
/*
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include <ngx_config.h>
#include <ngx_core.h>
#include <nginx.h>
#include <ngx_stream.h>


typedef enum {
    NGX_STREAM_ECHO_OPCODE_ECHO,        /* for both "echo" and
                                           "echo_duplicate" */
    NGX_STREAM_ECHO_OPCODE_SLEEP,       /* for "echo_sleep" */
    NGX_STREAM_ECHO_OPCODE_FLUSH_WAIT   /* for "echo_flush_wait" */
#if 0
    NGX_STREAM_ECHO_OPCODE_ECHO_READ_REQUEST,  /* TODO */
#endif
} ngx_stream_echo_opcode_t;


#pragma warning(disable : 4201)
typedef struct {
    union {
        ngx_str_t       buffer;
        ngx_msec_t      delay;
    };
    ngx_stream_echo_opcode_t      opcode;
} ngx_stream_echo_cmd_t;
#pragma warning(default : 4201)


typedef struct {
    ngx_array_t      cmds;  /* of elements of type ngx_stream_echo_cmd_t */
    ngx_msec_t       send_timeout;
#if 0
    ngx_msec_t       read_timeout;  /* unused */
#endif
} ngx_stream_echo_srv_conf_t;


typedef struct {
    ngx_uint_t                       cmd_index;

    ngx_chain_t                     *busy;
    ngx_chain_t                     *free;

    ngx_chain_writer_ctx_t           writer;

    ngx_event_t                      sleep;

    ngx_pool_cleanup_t              *cleanup;

    unsigned                         waiting_flush:1;
    unsigned                         done:1;
} ngx_stream_echo_ctx_t;


static void ngx_stream_echo_handler(ngx_stream_session_t *s);
static void ngx_stream_echo_resume_execution(ngx_stream_session_t *s);
static ngx_int_t ngx_stream_echo_run_cmds(ngx_stream_session_t *s);
static ngx_int_t ngx_stream_echo_eval_args(ngx_array_t *raw_args,
    ngx_uint_t init, ngx_array_t *args, ngx_array_t *opts);
static ngx_int_t ngx_stream_echo_exec_echo(ngx_stream_session_t *s,
    ngx_stream_echo_ctx_t *ctx, ngx_stream_echo_cmd_t *cmd);
static ngx_int_t ngx_stream_echo_exec_sleep(ngx_stream_session_t *s,
    ngx_stream_echo_ctx_t *ctx, ngx_stream_echo_cmd_t *cmd);
static void ngx_stream_echo_cleanup(void *data);
static ngx_int_t ngx_stream_echo_exec_flush_wait(ngx_stream_session_t *s,
    ngx_stream_echo_ctx_t *ctx, ngx_stream_echo_cmd_t *cmd);
static void ngx_stream_echo_writer(ngx_event_t *ev);
static void ngx_stream_echo_sleep_event_handler(ngx_event_t *ev);
static void ngx_stream_echo_block_reading(ngx_event_t *ev);
static void ngx_stream_echo_finalize_session(ngx_stream_session_t *s,
    ngx_int_t rc);
static ngx_stream_echo_ctx_t *
    ngx_stream_echo_create_ctx(ngx_stream_session_t *s);
static char *ngx_stream_echo_echo(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_stream_echo_cmd_t *ngx_stream_echo_helper(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf, ngx_stream_echo_opcode_t opcode,
    ngx_array_t *args, ngx_array_t *opts);
static char *ngx_stream_echo_echo_duplicate(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ssize_t ngx_stream_echo_atosz(u_char *line, size_t n);
static char *ngx_stream_echo_echo_sleep(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_stream_echo_echo_flush_wait(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static void *ngx_stream_echo_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_echo_merge_srv_conf(ngx_conf_t *cf,
    void *parent, void *child);


static ngx_command_t  ngx_stream_echo_commands[] = {

    { ngx_string("echo"),
      NGX_STREAM_SRV_CONF|NGX_CONF_ANY,
      ngx_stream_echo_echo,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("echo_duplicate"),
      NGX_STREAM_SRV_CONF|NGX_CONF_2MORE,
      ngx_stream_echo_echo_duplicate,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("echo_sleep"),
      NGX_STREAM_SRV_CONF|NGX_CONF_1MORE,
      ngx_stream_echo_echo_sleep,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("echo_flush_wait"),
      NGX_STREAM_SRV_CONF|NGX_CONF_ANY,
      ngx_stream_echo_echo_flush_wait,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("echo_send_timeout"),
      NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_echo_srv_conf_t, send_timeout),
      NULL },

      ngx_null_command
};


static ngx_stream_module_t  ngx_stream_echo_module_ctx = {
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    ngx_stream_echo_create_srv_conf,       /* create server configuration */
    ngx_stream_echo_merge_srv_conf         /* merge server configuration */
};


ngx_module_t  ngx_stream_echo_module = {
    NGX_MODULE_V1,
    &ngx_stream_echo_module_ctx,           /* module context */
    ngx_stream_echo_commands,              /* module directives */
    NGX_STREAM_MODULE,                     /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static void
ngx_stream_echo_handler(ngx_stream_session_t *s)
{
    ngx_connection_t            *c;
    ngx_stream_echo_ctx_t       *ctx;
    ngx_stream_echo_srv_conf_t  *escf;

    escf = ngx_stream_get_module_srv_conf(s, ngx_stream_echo_module);
    if (escf->cmds.nelts == 0) {
        ngx_stream_echo_finalize_session(s, NGX_DECLINED);
        return;
    }

    c = s->connection;

    c->write->handler = ngx_stream_echo_writer;
    c->read->handler = ngx_stream_echo_block_reading;

    ctx = ngx_stream_echo_create_ctx(s);
    if (ctx == NULL) {
        ngx_stream_echo_finalize_session(s, NGX_ERROR);
        return;
    }

    ngx_stream_set_ctx(s, ctx, ngx_stream_echo_module);

    ngx_stream_echo_resume_execution(s);
}


static void
ngx_stream_echo_resume_execution(ngx_stream_session_t *s)
{
    ngx_int_t       rc;

    rc = ngx_stream_echo_run_cmds(s);

    dd("run cmds returned %d", (int) rc);

    ngx_stream_echo_finalize_session(s, rc);
}


static ngx_int_t
ngx_stream_echo_run_cmds(ngx_stream_session_t *s)
{
    ngx_int_t                        rc;
    ngx_connection_t                *c;
    ngx_stream_echo_cmd_t           *cmd;
    ngx_stream_echo_ctx_t           *ctx;
    ngx_stream_echo_srv_conf_t      *escf;

    escf = ngx_stream_get_module_srv_conf(s, ngx_stream_echo_module);

    c = s->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream echo run commands");

    c->log->action = "running stream echo commands";

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_echo_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    cmd = escf->cmds.elts;

    while (ctx->cmd_index < escf->cmds.nelts) {

        dd("cmd indx: %d", (int) ctx->cmd_index);

        switch (cmd[ctx->cmd_index].opcode) {

        case NGX_STREAM_ECHO_OPCODE_ECHO:
            rc = ngx_stream_echo_exec_echo(s, ctx, &cmd[ctx->cmd_index]);
            break;

        case NGX_STREAM_ECHO_OPCODE_SLEEP:
            rc = ngx_stream_echo_exec_sleep(s, ctx, &cmd[ctx->cmd_index]);
            break;

        case NGX_STREAM_ECHO_OPCODE_FLUSH_WAIT:
            rc = ngx_stream_echo_exec_flush_wait(s, ctx, &cmd[ctx->cmd_index]);
            break;

        default:
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "stream echo unknown opcode: %d",
                          cmd[ctx->cmd_index].opcode);

            return NGX_ERROR;
        }

        ctx->cmd_index++;

        if (rc == NGX_ERROR || rc == NGX_DONE) {
            return rc;
        }
    }

    ctx->done = 1;

    return NGX_OK;
}


static ngx_int_t
ngx_stream_echo_eval_args(ngx_array_t *raw_args, ngx_uint_t init,
    ngx_array_t *args, ngx_array_t *opts)
{
    unsigned                         expecting_opts = 1;
    ngx_str_t                       *arg, *raw, *opt;
    ngx_str_t                       *value;
    ngx_uint_t                       i;

    value = raw_args->elts;

    for (i = init; i < raw_args->nelts; i++) {
        raw = &value[i];

        dd("checking raw arg: \"%.*s\"", (int) raw->len, raw->data);

        if (raw->len > 0) {
            if (expecting_opts) {
                if (raw->len == 1 || raw->data[0] != '-') {
                    expecting_opts = 0;

                } else if (raw->data[1] == '-') {
                    expecting_opts = 0;
                    continue;

                } else {
                    opt = ngx_array_push(opts);
                    if (opt == NULL) {
                        return NGX_ERROR;
                    }

                    opt->len = raw->len - 1;
                    opt->data = raw->data + 1;

                    dd("pushing opt: %.*s", (int) opt->len, opt->data);

                    continue;
                }
            }

        } else {
            expecting_opts = 0;
        }

        arg = ngx_array_push(args);
        if (arg == NULL) {
            return NGX_ERROR;
        }

        *arg = *raw;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_stream_echo_exec_echo(ngx_stream_session_t *s,
    ngx_stream_echo_ctx_t *ctx, ngx_stream_echo_cmd_t *cmd)
{
    ngx_int_t            rc;
    ngx_chain_t         *out;
    ngx_connection_t    *c;

    c = s->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream echo running echo");

    if (cmd->buffer.len == 0) {
        /* do nothing */
        return NGX_OK;
    }

    out = ngx_chain_get_free_buf(c->pool, &ctx->free);
    if (out == NULL) {
        return NGX_ERROR;
    }

    out->buf->memory = 1;

    out->buf->pos = cmd->buffer.data;
    out->buf->last = out->buf->pos + cmd->buffer.len;

    out->buf->tag = (ngx_buf_tag_t) &ngx_stream_echo_module;

    rc = ngx_chain_writer(&ctx->writer, out);

    dd("chain writer returned: %d", (int) rc);

    ngx_chain_update_chains(c->pool, &ctx->free, &ctx->busy, &out,
                            (ngx_buf_tag_t) &ngx_stream_echo_module);

    return rc;
}


static ngx_int_t
ngx_stream_echo_exec_sleep(ngx_stream_session_t *s,
    ngx_stream_echo_ctx_t *ctx, ngx_stream_echo_cmd_t *cmd)
{
    ngx_connection_t    *c;

    c = s->connection;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream echo running sleep (delay: %M)", cmd->delay);

    if (cmd->delay == 0) {
        /* do nothing */
        return NGX_OK;
    }

    if (ctx->cleanup == NULL) {
        ctx->cleanup = ngx_pool_cleanup_add(c->pool, 0);
        if (ctx->cleanup == NULL) {
            return NGX_ERROR;
        }

        ctx->cleanup->handler = ngx_stream_echo_cleanup;
        ctx->cleanup->data = ctx;
    }

    ngx_add_timer(&ctx->sleep, cmd->delay);

    return NGX_DONE;
}


static void
ngx_stream_echo_cleanup(void *data)
{
    ngx_stream_echo_ctx_t  *ctx = data;

    if (ctx->sleep.timer_set) {
        ngx_del_timer(&ctx->sleep);
    }
}


static ngx_int_t
ngx_stream_echo_exec_flush_wait(ngx_stream_session_t *s,
    ngx_stream_echo_ctx_t *ctx, ngx_stream_echo_cmd_t *cmd)
{
    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream echo running flush-wait (busy: %p)", ctx->busy);

    if (ctx->busy == NULL) {
        /* do nothing */
        return NGX_OK;
    }

    ctx->waiting_flush = 1;

    return NGX_DONE;
}


static void
ngx_stream_echo_writer(ngx_event_t *ev)
{
    ngx_int_t                    rc;
    ngx_chain_t                 *out;
    ngx_connection_t            *c;
    ngx_stream_session_t        *s;
    ngx_stream_echo_ctx_t       *ctx;
    ngx_stream_echo_srv_conf_t  *escf;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ev->log, 0,
                   "stream echo writer handler");

    c = ev->data;
    s = c->data;

    if (ev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "stream client send timed out");
        c->timedout = 1;

        ngx_stream_echo_finalize_session(s, NGX_ERROR);
        return;
    }

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_echo_module);
    if (ctx == NULL) {
        /* cannot really happen */
        return;
    }

    rc = ngx_chain_writer(&ctx->writer, NULL);

    out = NULL;
    ngx_chain_update_chains(c->pool, &ctx->free, &ctx->busy, &out,
                            (ngx_buf_tag_t) &ngx_stream_echo_module);

    if (rc == NGX_ERROR) {
        ngx_stream_echo_finalize_session(s, NGX_ERROR);
        return;
    }

    if (rc == NGX_AGAIN) {
        if (!c->write->ready) {
            escf = ngx_stream_get_module_srv_conf(s, ngx_stream_echo_module);

            ngx_add_timer(c->write, escf->send_timeout);

        } else if (c->write->timer_set) {
            ngx_del_timer(c->write);
        }

        if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
            ngx_stream_echo_finalize_session(s, NGX_ERROR);
        }

        return;
    }

    /* rc == NGX_OK */

    if (ctx->done) {
        /* all the commands are done */
        ngx_stream_echo_finalize_session(s, NGX_OK);
        return;
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    if (ctx->waiting_flush) {
        ctx->waiting_flush = 0;
        ngx_stream_echo_resume_execution(s);
        return;
    }

    ngx_stream_echo_finalize_session(s, NGX_DONE);
}


static void
ngx_stream_echo_sleep_event_handler(ngx_event_t *ev)
{
    ngx_connection_t        *c;
    ngx_stream_session_t    *s;

    s = ev->data;
    c = s->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream echo sleep event handler");

    if (c->destroyed) {
        return;
    }

    if (c->error) {
        ngx_stream_echo_finalize_session(s, NGX_ERROR);
        return;
    }

    if (!ev->timedout) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "stream echo: sleeping woke up prematurely");
        ngx_stream_echo_finalize_session(s, NGX_ERROR);
        return;
    }

    ev->timedout = 0;   /* reset for the next sleep (if any) */

    if (ev->timer_set) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "stream echo: timer not deleted on expiration");
        ngx_stream_echo_finalize_session(s, NGX_ERROR);
        return;
    }

    ngx_stream_echo_resume_execution(s);
}


static void
ngx_stream_echo_block_reading(ngx_event_t *ev)
{
    ngx_connection_t        *c;
    ngx_stream_session_t    *s;

    c = ev->data;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream echo reading blocked");

    if ((ngx_event_flags & NGX_USE_LEVEL_EVENT)
        && c->read->active)
    {
        if (ngx_del_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
            s = c->data;
            ngx_stream_echo_finalize_session(s, NGX_ERROR);
        }
    }
}


static void
ngx_stream_echo_finalize_session(ngx_stream_session_t *s,
    ngx_int_t rc)
{
    ngx_connection_t            *c;
    ngx_stream_echo_ctx_t       *ctx;
    ngx_stream_echo_srv_conf_t  *escf;

    c = s->connection;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream echo finalize session: rc=%i", rc);

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_echo_module);

    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        goto done;
    }

    if (ctx && ctx->busy) { /* having pending data to be sent */

        if (!c->write->ready) {
            escf = ngx_stream_get_module_srv_conf(s, ngx_stream_echo_module);

            ngx_add_timer(c->write, escf->send_timeout);

        } else if (c->write->timer_set) {
            ngx_del_timer(c->write);
        }

        if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
            ngx_stream_close_connection(c);
        }

        return;
    }

    if (rc == NGX_DONE) {   /* yield */

        if ((ngx_event_flags & NGX_USE_LEVEL_EVENT)
            && c->write->active)
        {
            dd("done: ctx->busy: %p", ctx->busy);

            if (ctx && ctx->busy == NULL) {
                if (ngx_del_event(c->write, NGX_WRITE_EVENT, 0) != NGX_OK) {
                    ngx_stream_close_connection(c);
                }
            }
        }

        return;
    }

    /* rc == NGX_OK || rc == NGX_AGAIN */

    if (ctx == NULL) {
        goto done;
    }

    dd("c->buffered: %d, busy: %p", (int) c->buffered, ctx->busy);

done:

    ngx_stream_close_connection(c);
    return;
}


static ngx_stream_echo_ctx_t *
ngx_stream_echo_create_ctx(ngx_stream_session_t *s)
{
    ngx_connection_t            *c;
    ngx_stream_echo_ctx_t       *ctx;

    c = s->connection;

    ctx = ngx_pcalloc(c->pool, sizeof(ngx_stream_echo_ctx_t));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->writer.pool = c->pool;
    ctx->writer.last = &ctx->writer.out;
    ctx->writer.connection = c;

    ctx->sleep.handler   = ngx_stream_echo_sleep_event_handler;
    ctx->sleep.data      = s;
    ctx->sleep.log       = c->log;

    /*
     * set by ngx_pcalloc():
     *
     *      ctx->cmd_index = 0;
     *      ctx->busy = NULL;
     *      ctx->free = NULL;
     *      ctx->writer.out = NULL;
     *      ctx->writer.limit = 0;
     *      ctx->cleanup = NULL;
     */

    return ctx;
}


static char *
ngx_stream_echo_echo(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char          *p;
    size_t           size;
    unsigned         nl;  /* controls whether to append a newline char */
    ngx_str_t       *opt, *arg;
    ngx_uint_t       i;
    ngx_array_t      opts, args;

    ngx_stream_echo_cmd_t     *echo_cmd;

    echo_cmd = ngx_stream_echo_helper(cf, cmd, conf,
                                      NGX_STREAM_ECHO_OPCODE_ECHO,
                                      &args, &opts);
    if (echo_cmd == NULL) {
        return NGX_CONF_ERROR;
    }

    /* handle options */

    nl = 1;
    opt = opts.elts;

    for (i = 0; i < opts.nelts; i++) {

        if (opt[i].len == 1 && opt[i].data[0] == 'n') {
            nl = 0;
            continue;
        }

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo sees unknown option \"-%*s\" "
                      "in \"echo\"", opt[i].len, opt[i].data);

        return NGX_CONF_ERROR;
    }

    /* prepare the data buffer to be sent.
     * TODO we could merge the data buffers of adjacent "echo" commands
     * further though it might not worth the trouble. oh well.
     */

    /* step 1: pre-calculate the total size of the data buffer actually
     * needed and allocate the buffer. */

    arg = args.elts;

    for (size = 0, i = 0; i < args.nelts; i++) {

        if (i > 0) {
            /* preserve a byte for prepending a space char */
            size++;
        }

        size += arg[i].len;
    }

    if (nl) {
        /* preserve a byte for the trailing newline char */
        size++;
    }

    if (size == 0) {

        echo_cmd->buffer.data = NULL;
        echo_cmd->buffer.len = 0;

        return NGX_CONF_OK;
    }

    p = ngx_palloc(cf->pool, size);
    if (p == NULL) {
        return NGX_CONF_ERROR;
    }

    echo_cmd->buffer.data = p;
    echo_cmd->buffer.len = size;

    /* step 2: fill in the buffer with actual data */

    for (i = 0; i < args.nelts; i++) {

        if (i > 0) {
            /* prepending a space char */
            *p++ = (u_char) ' ';
        }

        p = ngx_copy(p, arg[i].data, arg[i].len);
    }

    if (nl) {
        /* preserve a byte for the trailing newline char */
        *p++ = LF;
    }

    if (p - echo_cmd->buffer.data != (off_t) size) {  /* just an insurance */

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo internal buffer error: %O != %uz",
                      p - echo_cmd->buffer.data, size);

        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_stream_echo_cmd_t *
ngx_stream_echo_helper(ngx_conf_t *cf, ngx_command_t *cmd, void *conf,
    ngx_stream_echo_opcode_t opcode, ngx_array_t *args, ngx_array_t *opts)
{
    ngx_stream_echo_srv_conf_t  *escf = conf;

    ngx_stream_echo_cmd_t       *echo_cmd;
    ngx_stream_core_srv_conf_t  *cscf;

    if (escf->cmds.nelts == 0) {
        cscf = ngx_stream_conf_get_module_srv_conf(cf, ngx_stream_core_module);
        cscf->handler = ngx_stream_echo_handler;
    }

    echo_cmd = ngx_array_push(&escf->cmds);
    if (echo_cmd == NULL) {
        return NULL;
    }

    echo_cmd->opcode = opcode;

    if (ngx_array_init(args, cf->temp_pool, cf->args->nelts - 1,
                       sizeof(ngx_str_t))
        == NGX_ERROR)
    {
        return NULL;
    }

    if (ngx_array_init(opts, cf->temp_pool, 1, sizeof(ngx_str_t))
        == NGX_ERROR)
    {
        return NULL;
    }

    if (ngx_stream_echo_eval_args(cf->args, 1, args, opts) != NGX_OK) {
        return NULL;
    }

    return echo_cmd;
}


static char *
ngx_stream_echo_echo_duplicate(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char          *p;
    size_t           size;
    ssize_t          n;
    ngx_str_t       *opt, *arg;
    ngx_uint_t       i, i2;
    ngx_array_t      opts, args;

    ngx_stream_echo_cmd_t     *echo_cmd;

    /* we can just reuse the "echo" opcode for our purpose. */

    echo_cmd = ngx_stream_echo_helper(cf, cmd, conf,
                                      NGX_STREAM_ECHO_OPCODE_ECHO,
                                      &args, &opts);
    if (echo_cmd == NULL) {
        return NGX_CONF_ERROR;
    }

    /* handle options */

    opt = opts.elts;
    i2 = 0;

    for (i = 0; i < opts.nelts; i++) {

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo sees unknown option \"-%*s\" "
                      "in \"echo_duplicate\"", opt[i].len, opt[i].data);

        i2 = 1;
    }

    if (i2 > 0) {
        return NGX_CONF_ERROR;
    }

    if (args.nelts != 2) {

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo requires two value arguments in "
                      "\"echo_duplicate\" but got %ui", args.nelts);

        return NGX_CONF_ERROR;
    }

    /* prepare the data buffer to be sent */

    /* step 1: pre-calculate the total size of the data buffer actually
     * needed and allocate the buffer. */

    arg = args.elts;

    /* we do not use ngx_atosz here since we want to handle underscores in
     * the number representation. */
    n = ngx_stream_echo_atosz(arg[0].data, arg[0].len);

    if (n == NGX_ERROR) {

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo: bad \"n\" argument, \"%*s\", "
                      "in \"echo_duplicate\"", arg[0].len, arg[0].data);

        return NGX_CONF_ERROR;
    }

    size = n * arg[1].len;

    if (size == 0) {

        echo_cmd->buffer.data = NULL;
        echo_cmd->buffer.len = 0;

        return NGX_CONF_OK;
    }

    p = ngx_palloc(cf->pool, size);
    if (p == NULL) {
        return NGX_CONF_ERROR;
    }

    echo_cmd->buffer.data = p;
    echo_cmd->buffer.len = size;

    /* step 2: fill in the buffer with actual data */

    for (i = 0; (ssize_t) i < n; i++) {
        p = ngx_copy(p, arg[1].data, arg[1].len);
    }

    if (p - echo_cmd->buffer.data != (off_t) size) {  /* just an insurance */

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo internal buffer error: %O != %uz",
                      p - echo_cmd->buffer.data, size);

        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ssize_t
ngx_stream_echo_atosz(u_char *line, size_t n)
{
    ssize_t  value;

    if (n == 0) {
        return NGX_ERROR;
    }

    for (value = 0; n--; line++) {
        if (*line == '_') { /* we ignore undercores */
            continue;
        }

        if (*line < '0' || *line > '9') {
            return NGX_ERROR;
        }

        value = value * 10 + (*line - '0');
    }

    if (value < 0) {
        return NGX_ERROR;
    }

    return value;
}


static char *
ngx_stream_echo_echo_sleep(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t       *opt, *arg;
    ngx_int_t        delay;  /* in msec */
    ngx_uint_t       i, i2;
    ngx_array_t      opts, args;

    ngx_stream_echo_cmd_t     *echo_cmd;

    echo_cmd = ngx_stream_echo_helper(cf, cmd, conf,
                                      NGX_STREAM_ECHO_OPCODE_SLEEP,
                                      &args, &opts);
    if (echo_cmd == NULL) {
        return NGX_CONF_ERROR;
    }

    /* handle options */

    opt = opts.elts;
    i2 = 0;

    for (i = 0; i < opts.nelts; i++) {

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo sees unknown option \"-%*s\" "
                      "in \"echo_sleep\"", opt[i].len, opt[i].data);

        i2 = 1;
    }

    if (i2 > 0) {
        return NGX_CONF_ERROR;
    }

    if (args.nelts != 1) {

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo requires one value argument in "
                      "\"echo_sleep\" but got %ui", args.nelts);

        return NGX_CONF_ERROR;
    }

    arg = args.elts;

    delay = ngx_atofp(arg[0].data, arg[0].len, 3);

    if (delay == NGX_ERROR) {

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo: bad \"delay\" argument, \"%*s\", "
                      "in \"echo_sleep\"", arg[0].len, arg[0].data);

        return NGX_CONF_ERROR;
    }

    dd("sleep delay: %d", (int) delay);

    echo_cmd->delay = (ngx_msec_t) delay;

    return NGX_CONF_OK;
}


static char *
ngx_stream_echo_echo_flush_wait(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t       *opt;
    ngx_uint_t       i, i2;
    ngx_array_t      opts, args;

    ngx_stream_echo_cmd_t     *echo_cmd;

    echo_cmd = ngx_stream_echo_helper(cf, cmd, conf,
                                      NGX_STREAM_ECHO_OPCODE_FLUSH_WAIT,
                                      &args, &opts);
    if (echo_cmd == NULL) {
        return NGX_CONF_ERROR;
    }

    /* handle options */

    opt = opts.elts;
    i2 = 0;

    for (i = 0; i < opts.nelts; i++) {

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo sees unknown option \"-%*s\" "
                      "in \"echo_flush_wait\"", opt[i].len, opt[i].data);

        i2 = 1;
    }

    if (i2 > 0) {
        return NGX_CONF_ERROR;
    }

    if (args.nelts != 0) {

        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "stream echo takes no value arguments in "
                      "\"echo_flush_wait\" but got %ui", args.nelts);

        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static void *
ngx_stream_echo_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_echo_srv_conf_t  *conf;

    conf = ngx_palloc(cf->pool, sizeof(ngx_stream_echo_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&conf->cmds, cf->pool, 1,
                       sizeof(ngx_stream_echo_cmd_t))
        == NGX_ERROR)
    {
        return NULL;
    }

    conf->send_timeout = NGX_CONF_UNSET_MSEC;

    return conf;
}


static char *
ngx_stream_echo_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_stream_echo_srv_conf_t *prev = parent;
    ngx_stream_echo_srv_conf_t *conf = child;

#if 0
    if (conf->cmds.nelts == 0 && prev->cmds.nelts > 0) {
        /* assuming that these arrays are read-only afterwards */
        ngx_memcpy(&conf->cmds, &prev->cmds, sizeof(ngx_array_t));
    }
#endif

    ngx_conf_merge_msec_value(conf->send_timeout, prev->send_timeout, 60000);

    return NGX_CONF_OK;
}
