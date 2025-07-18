
/*
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>

#ifndef OPENSSL_NO_ECH
#include <ngx_stream_ssl_preread_module.h>
#else
/*
 * moved these to a header to allow access from elsewhere
 * which is needed for ECH split-mode with HRR
 */
typedef struct {
    ngx_flag_t      enabled;
#ifndef OPENSSL_NO_ECH
    ngx_str_t       echkeydir;
    ngx_ssl_t       *ssl;
#endif
} ngx_stream_ssl_preread_srv_conf_t;


typedef struct {
    size_t          left;
    size_t          size;
    size_t          ext;
    u_char         *pos;
    u_char         *dst;
    u_char          buf[4];
    u_char          version[2];
    ngx_str_t       host;
    ngx_str_t       alpn;
    ngx_log_t      *log;
    ngx_pool_t     *pool;
    ngx_uint_t      state;
#ifndef OPENSSL_NO_ECH
    ngx_uint_t      ech_state;
    u_char          *hrrtok;
    size_t          hrrtoklen;
#endif
} ngx_stream_ssl_preread_ctx_t;
#endif

static ngx_int_t ngx_stream_ssl_preread_handler(ngx_stream_session_t *s);
static ngx_int_t ngx_stream_ssl_preread_parse_record(
    ngx_stream_ssl_preread_ctx_t *ctx, u_char *pos, u_char *last);
static ngx_int_t ngx_stream_ssl_preread_servername(ngx_stream_session_t *s,
    ngx_str_t *servername);
static ngx_int_t ngx_stream_ssl_preread_protocol_variable(
    ngx_stream_session_t *s, ngx_stream_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_stream_ssl_preread_server_name_variable(
    ngx_stream_session_t *s, ngx_stream_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_stream_ssl_preread_alpn_protocols_variable(
    ngx_stream_session_t *s, ngx_stream_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_stream_ssl_preread_add_variables(ngx_conf_t *cf);
static void *ngx_stream_ssl_preread_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_ssl_preread_merge_srv_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_stream_ssl_preread_init(ngx_conf_t *cf);


static ngx_command_t  ngx_stream_ssl_preread_commands[] = {

    { ngx_string("ssl_preread"),
      NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_ssl_preread_srv_conf_t, enabled),
      NULL },

#ifndef OPENSSL_NO_ECH
    { ngx_string("ssl_echkeydir"),
      NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_ssl_preread_srv_conf_t, echkeydir),
      NULL },
#endif

      ngx_null_command
};


static ngx_stream_module_t  ngx_stream_ssl_preread_module_ctx = {
    ngx_stream_ssl_preread_add_variables,   /* preconfiguration */
    ngx_stream_ssl_preread_init,            /* postconfiguration */

    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */

    ngx_stream_ssl_preread_create_srv_conf, /* create server configuration */
    ngx_stream_ssl_preread_merge_srv_conf   /* merge server configuration */
};


ngx_module_t  ngx_stream_ssl_preread_module = {
    NGX_MODULE_V1,
    &ngx_stream_ssl_preread_module_ctx,     /* module context */
    ngx_stream_ssl_preread_commands,        /* module directives */
    NGX_STREAM_MODULE,                      /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_stream_variable_t  ngx_stream_ssl_preread_vars[] = {

    { ngx_string("ssl_preread_protocol"), NULL,
      ngx_stream_ssl_preread_protocol_variable, 0, 0, 0 },

    { ngx_string("ssl_preread_server_name"), NULL,
      ngx_stream_ssl_preread_server_name_variable, 0, 0, 0 },

    { ngx_string("ssl_preread_alpn_protocols"), NULL,
      ngx_stream_ssl_preread_alpn_protocols_variable, 0, 0, 0 },

      ngx_stream_null_variable
};


#ifndef OPENSSL_NO_ECH
ngx_int_t ngx_stream_do_ech(
    ngx_stream_ssl_preread_srv_conf_t  *sscf,
    ngx_stream_ssl_preread_ctx_t       *ctx,
    ngx_connection_t                   *c,
    u_char                             *p,
    u_char                             **last,
    int                                *dec_ok)
{
    int rv = 0;
    char *inner_sni = NULL, *outer_sni = NULL;
    unsigned char *hrrtok = NULL, *chstart = NULL, *inp = NULL;
    size_t toklen = 0, chlen = 0, msglen = 0, innerlen = 0;

    ngx_ssl_error(NGX_LOG_NOTICE, c->log, 0, "do_ech: checking ECH");
    chstart = p;
    /*
     * chew up bogus change cipher spec, if one's there when we're
     * in midst of HRR
     */
    if (ctx->ech_state == 1
        && p[0] == 20
        && p[1] == 3
        && p[2] == 3
        && p[3] == 0
        && p[4] == 1
        && p[5] == 1) {
        chstart += 6;
        ngx_ssl_error(NGX_LOG_NOTICE, c->log, 0, "do_ech: skipped CCS");
    }
    /*
     * check that we are dealing with a TLSv1.3 ClientHello
     * and establish CH length (in case of early data)
     */
    if (chstart[0] == 22 /* handshake */
        && chstart[1] == 3 /* tls 1.2 or 1.3 */
        && (chstart[2] == 3 || chstart[2] == 2 || chstart[2] == 1) /* all ok */
        && chstart[5] == 1) { /* ClientHello */

        chlen = 5 + (uint8_t)chstart[3] * 256 + (uint8_t)chstart[4];
        msglen = *last - chstart; /* in case of early data */
        if (msglen != chlen) {
            ngx_ssl_error(NGX_LOG_NOTICE, c->log, 0,
                "do_ech: message (%z) longer than CH (%z)", msglen, chlen);
        } else {
            ngx_ssl_error(NGX_LOG_NOTICE, c->log, 0,
                "do_ech: message only has CH (%z)", chlen);
        }
        /* we'll at least try for ECH */
        inp = ngx_pcalloc(c->pool, chlen);
        if (inp == NULL) {
            return NGX_ERROR;
        }
        innerlen = chlen;
        if (ctx->ech_state != 0) {
            ngx_ssl_error(NGX_LOG_NOTICE, c->log, 0,
                "do_ech: ECH 2nd go (HRR), toklen = %d", (int)ctx->hrrtoklen);
            hrrtok = ctx->hrrtok;
            toklen = ctx->hrrtoklen;
        } else {
            ngx_ssl_error(NGX_LOG_NOTICE, c->log, 0, "do_ech: ECH 1st time");
        }
        rv = SSL_CTX_ech_raw_decrypt(sscf->ssl->ctx, dec_ok,
                                    &inner_sni, &outer_sni,
                                    chstart, chlen,
                                    inp, &innerlen,
                                    &hrrtok, &toklen);
        if (ctx->ech_state == 1) {
            OPENSSL_free(hrrtok); /* we can free that now */
            ctx->hrrtok = NULL;
        }
        if (rv != 1) {
            ngx_ssl_error(NGX_LOG_NOTICE, c->log, 0,
                "do_ech: ECH decrypt failed (%d)", rv);
            OPENSSL_free(inner_sni);
            OPENSSL_free(outer_sni);
            return NGX_ERROR;
        }
        if (*dec_ok == 1) {
            ngx_ssl_error(NGX_LOG_NOTICE, c->log, 0,
                "do_ech: ECH success outer_sni: %s inner_sni: %s",
                (outer_sni?outer_sni:"NONE"),(inner_sni?inner_sni:"NONE"));
            if (ctx->ech_state == 0) {
                /* store hrrtok from 1st CH, in case needed later */
                ctx->hrrtok = hrrtok;
                ctx->hrrtoklen = toklen;
            }
            /* increment ech_state */
            ctx->ech_state += 1;
            /* swap CH's over, adding back extra data if any */
            memcpy(chstart, inp, innerlen);
            if (msglen > chlen) {
                memcpy(chstart + innerlen, chstart + chlen, msglen - chlen);
                *last = chstart + innerlen + (msglen - chlen);
            } else {
                *last = chstart + innerlen;
            }
            c->buffer->last = *last;
        } else {
            ngx_ssl_error(NGX_LOG_NOTICE, c->log, 0,
                "do_ech: ECH decrypt failed (%d)", rv);
        }
        OPENSSL_free(inner_sni);
        OPENSSL_free(outer_sni);
    } else {
        ngx_ssl_error(NGX_LOG_NOTICE, c->log, 0,
            "do_ech: not a CH or CCS, contentype: %z, h/s type: %z",
            chstart[0], chstart[5]);
    }
    return NGX_OK;
}
#endif

static ngx_int_t
ngx_stream_ssl_preread_handler(ngx_stream_session_t *s)
{
    u_char                             *last, *p;
    size_t                              len;
    ngx_int_t                           rc;
    ngx_connection_t                   *c;
    ngx_stream_ssl_preread_ctx_t       *ctx;
    ngx_stream_ssl_preread_srv_conf_t  *sscf;

    c = s->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0, "ssl preread handler");

    sscf = ngx_stream_get_module_srv_conf(s, ngx_stream_ssl_preread_module);

    if (!sscf->enabled) {
        return NGX_DECLINED;
    }

    if (c->type != SOCK_STREAM) {
        return NGX_DECLINED;
    }

    if (c->buffer == NULL) {
        return NGX_AGAIN;
    }

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_ssl_preread_module);
    if (ctx == NULL) {
        ctx = ngx_pcalloc(c->pool, sizeof(ngx_stream_ssl_preread_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        ngx_stream_set_ctx(s, ctx, ngx_stream_ssl_preread_module);

        ctx->pool = c->pool;
        ctx->log = c->log;
        ctx->pos = c->buffer->pos;
#ifndef OPENSSL_NO_ECH
        ctx->hrrtok = NULL;
        ctx->hrrtoklen = 0;
        ctx->ech_state = 0;
#endif

    }

    p = ctx->pos;
    last = c->buffer->last;

#ifndef OPENSSL_NO_ECH
    /* check if we're trying ECH */
    if (sscf->ssl != NULL && sscf->ssl->ctx != NULL) {
        int echrv, dec_ok = 0;

        echrv = ngx_stream_do_ech(sscf, ctx, c, p, &last, &dec_ok);
        if (echrv != NGX_OK) {
            return NGX_ERROR;
        }
    }
#endif

    while (last - p >= 5) {

        if ((p[0] & 0x80) && p[2] == 1 && (p[3] == 0 || p[3] == 3)) {
            ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                           "ssl preread: version 2 ClientHello");
            ctx->version[0] = p[3];
            ctx->version[1] = p[4];
            return NGX_OK;
        }

        if (p[0] != 0x16) {
            ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                           "ssl preread: not a handshake");
            ngx_stream_set_ctx(s, NULL, ngx_stream_ssl_preread_module);
            return NGX_DECLINED;
        }

        if (p[1] != 3) {
            ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                           "ssl preread: unsupported SSL version");
            ngx_stream_set_ctx(s, NULL, ngx_stream_ssl_preread_module);
            return NGX_DECLINED;
        }

        len = (p[3] << 8) + p[4];

        /* read the whole record before parsing */
        if ((size_t) (last - p) < len + 5) {
            break;
        }

        p += 5;

        rc = ngx_stream_ssl_preread_parse_record(ctx, p, p + len);

        if (rc == NGX_DECLINED) {
            ngx_stream_set_ctx(s, NULL, ngx_stream_ssl_preread_module);
            return NGX_DECLINED;
        }

        if (rc == NGX_OK) {
            return ngx_stream_ssl_preread_servername(s, &ctx->host);
        }

        if (rc != NGX_AGAIN) {
            return rc;
        }

        p += len;
    }

    ctx->pos = p;

    return NGX_AGAIN;
}


static ngx_int_t
ngx_stream_ssl_preread_parse_record(ngx_stream_ssl_preread_ctx_t *ctx,
    u_char *pos, u_char *last)
{
    size_t   left, n, size, ext;
    u_char  *dst, *p;

    enum {
        sw_start = 0,
        sw_header,          /* handshake msg_type, length */
        sw_version,         /* client_version */
        sw_random,          /* random */
        sw_sid_len,         /* session_id length */
        sw_sid,             /* session_id */
        sw_cs_len,          /* cipher_suites length */
        sw_cs,              /* cipher_suites */
        sw_cm_len,          /* compression_methods length */
        sw_cm,              /* compression_methods */
        sw_ext,             /* extension */
        sw_ext_header,      /* extension_type, extension_data length */
        sw_sni_len,         /* SNI length */
        sw_sni_host_head,   /* SNI name_type, host_name length */
        sw_sni_host,        /* SNI host_name */
        sw_alpn_len,        /* ALPN length */
        sw_alpn_proto_len,  /* ALPN protocol_name length */
        sw_alpn_proto_data, /* ALPN protocol_name */
        sw_supver_len       /* supported_versions length */
    } state;

    ngx_log_debug2(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                   "ssl preread: state %ui left %z", ctx->state, ctx->left);

    state = ctx->state;
    size = ctx->size;
    left = ctx->left;
    ext = ctx->ext;
    dst = ctx->dst;
    p = ctx->buf;

    for ( ;; ) {
        n = ngx_min((size_t) (last - pos), size);

        if (dst) {
            dst = ngx_cpymem(dst, pos, n);
        }

        pos += n;
        size -= n;
        left -= n;

        if (size != 0) {
            break;
        }

        switch (state) {

        case sw_start:
            state = sw_header;
            dst = p;
            size = 4;
            left = size;
            break;

        case sw_header:
            if (p[0] != 1) {
                ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                               "ssl preread: not a client hello");
                return NGX_DECLINED;
            }

            state = sw_version;
            dst = ctx->version;
            size = 2;
            left = (p[1] << 16) + (p[2] << 8) + p[3];
            break;

        case sw_version:
            state = sw_random;
            dst = NULL;
            size = 32;
            break;

        case sw_random:
            state = sw_sid_len;
            dst = p;
            size = 1;
            break;

        case sw_sid_len:
            state = sw_sid;
            dst = NULL;
            size = p[0];
            break;

        case sw_sid:
            state = sw_cs_len;
            dst = p;
            size = 2;
            break;

        case sw_cs_len:
            state = sw_cs;
            dst = NULL;
            size = (p[0] << 8) + p[1];
            break;

        case sw_cs:
            state = sw_cm_len;
            dst = p;
            size = 1;
            break;

        case sw_cm_len:
            state = sw_cm;
            dst = NULL;
            size = p[0];
            break;

        case sw_cm:
            if (left == 0) {
                /* no extensions */
                return NGX_OK;
            }

            state = sw_ext;
            dst = p;
            size = 2;
            break;

        case sw_ext:
            if (left == 0) {
                return NGX_OK;
            }

            state = sw_ext_header;
            dst = p;
            size = 4;
            break;

        case sw_ext_header:
            if (p[0] == 0 && p[1] == 0 && ctx->host.data == NULL) {
                /* SNI extension */
                state = sw_sni_len;
                dst = p;
                size = 2;
                break;
            }

            if (p[0] == 0 && p[1] == 16 && ctx->alpn.data == NULL) {
                /* ALPN extension */
                state = sw_alpn_len;
                dst = p;
                size = 2;
                break;
            }

            if (p[0] == 0 && p[1] == 43) {
                /* supported_versions extension */
                state = sw_supver_len;
                dst = p;
                size = 1;
                break;
            }

            state = sw_ext;
            dst = NULL;
            size = (p[2] << 8) + p[3];
            break;

        case sw_sni_len:
            ext = (p[0] << 8) + p[1];
            state = sw_sni_host_head;
            dst = p;
            size = 3;
            break;

        case sw_sni_host_head:
            if (p[0] != 0) {
                ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                               "ssl preread: SNI hostname type is not DNS");
                return NGX_DECLINED;
            }

            size = (p[1] << 8) + p[2];

            if (ext < 3 + size) {
                ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                               "ssl preread: SNI format error");
                return NGX_DECLINED;
            }
            ext -= 3 + size;

            ctx->host.data = ngx_pnalloc(ctx->pool, size);
            if (ctx->host.data == NULL) {
                return NGX_ERROR;
            }

            state = sw_sni_host;
            dst = ctx->host.data;
            break;

        case sw_sni_host:
            ctx->host.len = (p[1] << 8) + p[2];

            state = sw_ext;
            dst = NULL;
            size = ext;
            break;

        case sw_alpn_len:
            ext = (p[0] << 8) + p[1];

            ctx->alpn.data = ngx_pnalloc(ctx->pool, ext);
            if (ctx->alpn.data == NULL) {
                return NGX_ERROR;
            }

            state = sw_alpn_proto_len;
            dst = p;
            size = 1;
            break;

        case sw_alpn_proto_len:
            size = p[0];

            if (size == 0) {
                ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                               "ssl preread: ALPN empty protocol");
                return NGX_DECLINED;
            }

            if (ext < 1 + size) {
                ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                               "ssl preread: ALPN format error");
                return NGX_DECLINED;
            }
            ext -= 1 + size;

            state = sw_alpn_proto_data;
            dst = ctx->alpn.data + ctx->alpn.len;
            break;

        case sw_alpn_proto_data:
            ctx->alpn.len += p[0];

            ngx_log_debug1(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                           "ssl preread: ALPN protocols \"%V\"", &ctx->alpn);

            if (ext) {
                ctx->alpn.data[ctx->alpn.len++] = ',';

                state = sw_alpn_proto_len;
                dst = p;
                size = 1;
                break;
            }

            state = sw_ext;
            dst = NULL;
            size = 0;
            break;

        case sw_supver_len:
            ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                           "ssl preread: supported_versions");

            /* set TLSv1.3 */
            ctx->version[0] = 3;
            ctx->version[1] = 4;

            state = sw_ext;
            dst = NULL;
            size = p[0];
            break;
        }

        if (left < size) {
            ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ctx->log, 0,
                           "ssl preread: failed to parse handshake");
            return NGX_DECLINED;
        }
    }

    ctx->state = state;
    ctx->size = size;
    ctx->left = left;
    ctx->ext = ext;
    ctx->dst = dst;

    return NGX_AGAIN;
}


static ngx_int_t
ngx_stream_ssl_preread_servername(ngx_stream_session_t *s,
    ngx_str_t *servername)
{
    ngx_int_t                    rc;
    ngx_str_t                    host;
    ngx_connection_t            *c;
    ngx_stream_core_srv_conf_t  *cscf;

    c = s->connection;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "SSL preread server name: \"%V\"", servername);

    if (servername->len == 0) {
        return NGX_OK;
    }

    host = *servername;

    rc = ngx_stream_validate_host(&host, c->pool, 0);

    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (rc == NGX_DECLINED) {
        return NGX_OK;
    }

    rc = ngx_stream_find_virtual_server(s, &host, &cscf);

    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (rc == NGX_DECLINED) {
        return NGX_OK;
    }

    s->srv_conf = cscf->ctx->srv_conf;

    ngx_set_connection_log(c, cscf->error_log);

    return NGX_OK;
}


static ngx_int_t
ngx_stream_ssl_preread_protocol_variable(ngx_stream_session_t *s,
    ngx_variable_value_t *v, uintptr_t data)
{
    ngx_str_t                      version;
    ngx_stream_ssl_preread_ctx_t  *ctx;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_ssl_preread_module);

    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    /* SSL_get_version() format */

    ngx_str_null(&version);

    switch (ctx->version[0]) {
    case 0:
        switch (ctx->version[1]) {
        case 2:
            ngx_str_set(&version, "SSLv2");
            break;
        }
        break;
    case 3:
        switch (ctx->version[1]) {
        case 0:
            ngx_str_set(&version, "SSLv3");
            break;
        case 1:
            ngx_str_set(&version, "TLSv1");
            break;
        case 2:
            ngx_str_set(&version, "TLSv1.1");
            break;
        case 3:
            ngx_str_set(&version, "TLSv1.2");
            break;
        case 4:
            ngx_str_set(&version, "TLSv1.3");
            break;
        }
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = version.len;
    v->data = version.data;

    return NGX_OK;
}


static ngx_int_t
ngx_stream_ssl_preread_server_name_variable(ngx_stream_session_t *s,
    ngx_variable_value_t *v, uintptr_t data)
{
    ngx_stream_ssl_preread_ctx_t  *ctx;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_ssl_preread_module);

    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = ctx->host.len;
    v->data = ctx->host.data;

    return NGX_OK;
}


static ngx_int_t
ngx_stream_ssl_preread_alpn_protocols_variable(ngx_stream_session_t *s,
    ngx_variable_value_t *v, uintptr_t data)
{
    ngx_stream_ssl_preread_ctx_t  *ctx;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_ssl_preread_module);

    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = ctx->alpn.len;
    v->data = ctx->alpn.data;

    return NGX_OK;
}


static ngx_int_t
ngx_stream_ssl_preread_add_variables(ngx_conf_t *cf)
{
    ngx_stream_variable_t  *var, *v;

    for (v = ngx_stream_ssl_preread_vars; v->name.len; v++) {
        var = ngx_stream_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static void *
ngx_stream_ssl_preread_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_ssl_preread_srv_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_ssl_preread_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->enabled = NGX_CONF_UNSET;
#ifndef OPENSSL_NO_ECH
    memset(&conf->echkeydir, 0, sizeof(conf->echkeydir));
    conf->ssl = NULL;
#endif

    return conf;
}


static char *
ngx_stream_ssl_preread_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_stream_ssl_preread_srv_conf_t *prev = parent;
    ngx_stream_ssl_preread_srv_conf_t *conf = child;

    ngx_conf_merge_value(conf->enabled, prev->enabled, 0);
#ifndef OPENSSL_NO_ECH
    ngx_conf_merge_str_value(conf->echkeydir, prev->echkeydir, "");
    if (ngx_strcmp(conf->echkeydir.data, "") != 0) {
        const SSL_METHOD *meth = NULL;
        SSL_CTX *sctx = NULL;

        meth = TLS_server_method();
        if (cf == NULL || cf->log == NULL || meth == NULL) {
            return NULL;
        }
        sctx = SSL_CTX_new(meth);
        if (sctx == NULL) {
            return NULL;
        }
        conf->ssl = ngx_pcalloc(cf->pool, sizeof(ngx_ssl_t));
        if (conf->ssl == NULL) {
            return NULL;
        }
        conf->ssl->ctx = sctx;
        conf->ssl->log = cf->log;
        if (ngx_ssl_echkeydir(cf, conf->ssl, &conf->echkeydir) != NGX_OK) {
            return NULL;
        }
    }
#endif

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_stream_ssl_preread_init(ngx_conf_t *cf)
{
    ngx_stream_handler_pt        *h;
    ngx_stream_core_main_conf_t  *cmcf;

    cmcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_STREAM_PREREAD_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_stream_ssl_preread_handler;

    return NGX_OK;
}
