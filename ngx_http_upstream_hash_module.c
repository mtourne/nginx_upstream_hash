/*
 * Hash a variable to choose an upstream server.
 *
 * Copyright (C) Evan Miller
 *
 * This module can be distributed under the same terms as Nginx itself.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#if (NGX_UPSTREAM_CHECK_MODULE)
# include "ngx_http_upstream_check_handler.h"
#endif

#define ngx_bitvector_index(index) (index / (8 * sizeof(uintptr_t)))
#define ngx_bitvector_bit(index) ((uintptr_t) 1 << (index % (8 * sizeof(uintptr_t))))

typedef struct {
    struct sockaddr                *sockaddr;
    socklen_t                       socklen;
    ngx_str_t                       name;
#if (NGX_HTTP_SSL)
    ngx_ssl_session_t              *ssl_session;   /* local to a process */
#endif

#if (NGX_UPSTREAM_CHECK_MODULE)
    ngx_uint_t                      check_index;
#endif
} ngx_http_upstream_hash_peer_t;

typedef struct {
    ngx_uint_t                        number;
    ngx_http_upstream_hash_peer_t     peer[0];
} ngx_http_upstream_hash_peers_t;

typedef struct {
    ngx_http_upstream_hash_peers_t   *peers;
    uint32_t                          hash;
    ngx_str_t                         current_key;
    ngx_str_t                         original_key;
    ngx_uint_t                        try_i;
    uintptr_t                         tried[1];
    ngx_uint_t                        peer_index;
} ngx_http_upstream_hash_peer_data_t;


static void ngx_http_upstream_hash_next_peer(ngx_http_upstream_hash_peer_data_t *uhpd,
        ngx_uint_t *tries, ngx_log_t *log);
static ngx_int_t ngx_http_upstream_init_hash_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_get_hash_peer(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_free_hash_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t state);
#if (NGX_HTTP_SSL)
static ngx_int_t ngx_http_upstream_set_hash_peer_session(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_save_hash_peer_session(ngx_peer_connection_t *pc,
    void *data);
#endif
static char *ngx_http_upstream_hash(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_upstream_hash_again(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_upstream_init_hash(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);
static ngx_uint_t ngx_http_upstream_hash_crc32(u_char *keydata, size_t keylen);


static ngx_command_t  ngx_http_upstream_hash_commands[] = {
    { ngx_string("hash"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_hash,
      0,
      0,
      NULL },

    { ngx_string("hash_again"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_hash_again,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_upstream_hash_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_upstream_hash_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_hash_module_ctx,    /* module context */
    ngx_http_upstream_hash_commands,       /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_upstream_init_hash(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
    ngx_uint_t                       i, j, n;
    ngx_http_upstream_server_t      *server;
    ngx_http_upstream_hash_peers_t  *peers;

    us->peer.init = ngx_http_upstream_init_hash_peer;

    if (!us->servers) {

        return NGX_ERROR;
    }

    server = us->servers->elts;

    for (n = 0, i = 0; i < us->servers->nelts; i++) {
        n += server[i].naddrs;
    }

    peers = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_hash_peers_t)
            + sizeof(ngx_http_upstream_hash_peer_t) * n);

    if (peers == NULL) {
        return NGX_ERROR;
    }

    peers->number = n;

    /* one hostname can have multiple IP addresses in DNS */
    for (n = 0, i = 0; i < us->servers->nelts; i++) {
        for (j = 0; j < server[i].naddrs; j++, n++) {
            peers->peer[n].sockaddr = server[i].addrs[j].sockaddr;
            peers->peer[n].socklen = server[i].addrs[j].socklen;
            peers->peer[n].name = server[i].addrs[j].name;
#if (NGX_UPSTREAM_CHECK_MODULE)
            if (!server[i].down) {
                peers->peer[n].check_index =
                    ngx_http_check_add_peer(cf, us, &server[i].addrs[j]);
            }
            else {
                peers->peer[n].check_index = (ngx_uint_t) NGX_ERROR;
            }
#endif
        }
    }

    us->peer.data = peers;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_init_hash_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_hash_peer_data_t     *uhpd;

    ngx_str_t val;

    if (ngx_http_script_run(r, &val, us->lengths, 0, us->values) == NULL) {
        return NGX_ERROR;
    }

    uhpd = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_hash_peer_data_t)
            + sizeof(uintptr_t) *
                ((ngx_http_upstream_hash_peers_t *)us->peer.data)->number /
                    (8 * sizeof(uintptr_t)));
    if (uhpd == NULL) {
        return NGX_ERROR;
    }

    r->upstream->peer.data = uhpd;

    uhpd->peers = us->peer.data;

    r->upstream->peer.free = ngx_http_upstream_free_hash_peer;
    r->upstream->peer.get = ngx_http_upstream_get_hash_peer;
    r->upstream->peer.tries = us->retries + 1;
#if (NGX_HTTP_SSL)
    r->upstream->peer.set_session = ngx_http_upstream_set_hash_peer_session;
    r->upstream->peer.save_session = ngx_http_upstream_save_hash_peer_session;
#endif

    /* must be big enough for the retry keys */
    if ((uhpd->current_key.data = ngx_pcalloc(r->pool, NGX_ATOMIC_T_LEN + val.len)) == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(uhpd->current_key.data, val.data, val.len);
    uhpd->current_key.len = val.len;
    uhpd->original_key = val;
    uhpd->hash = ngx_http_upstream_hash_crc32(uhpd->current_key.data, uhpd->current_key.len);
    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "upstream_hash: hashed \"%V\" to %ui", &uhpd->current_key, uhpd->hash % uhpd->peers->number);
    uhpd->try_i = 0;

    /* In case this one is marked down */
    ngx_http_upstream_hash_next_peer(uhpd, &r->upstream->peer.tries, r->connection->log);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "upstream_hash: Starting with %ui", uhpd->hash % uhpd->peers->number);


    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_get_hash_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_hash_peer_data_t  *uhpd = data;
    ngx_http_upstream_hash_peer_t       *peer;
    ngx_uint_t                           peer_index;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "upstream_hash: get upstream request hash peer try %ui", pc->tries);

    pc->cached = 0;
    pc->connection = NULL;

    peer_index = uhpd->peer_index;

    peer = &uhpd->peers->peer[peer_index];


    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "upstream_hash: chose peer %ui w/ hash %ui for tries %ui", peer_index, uhpd->hash, pc->tries);

    pc->sockaddr = peer->sockaddr;
    pc->socklen = peer->socklen;
    pc->name = &peer->name;

    return NGX_OK;
}

/* retry implementation is PECL memcache compatible */
static void
ngx_http_upstream_free_hash_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    ngx_http_upstream_hash_peer_data_t  *uhpd = data;
    ngx_uint_t                           current;
#if (NGX_UPSTREAM_CHECK_MODULE)
    ngx_http_upstream_hash_peer_t       *peer = NULL;
    ngx_uint_t                           start_index;
#endif

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
            "upstream_hash: free upstream hash peer try %ui", pc->tries);

    if (state & (NGX_PEER_FAILED|NGX_PEER_NEXT)
            && pc->tries) {
        /* Host that was supposed to be UP is DOWN
         * revert to the hash_again method to find a valid peer
         */
        current = uhpd->peer_index;

        uhpd->tried[ngx_bitvector_index(current)] |= ngx_bitvector_bit(current);
        ngx_http_upstream_hash_next_peer(uhpd, &pc->tries, pc->log);
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
          "upstream_hash: chose peer: %ui because: %ui failed", uhpd->peer_index, current);
    } else {
        current = uhpd->hash % uhpd->peers->number;
#if (NGX_UPSTREAM_CHECK_MODULE)
        start_index = current;
        peer = &uhpd->peers->peer[current];

        /* cycle through peers until a peer UP is found */
        while (ngx_http_check_peer_down(peer->check_index)) {
                current = (current + 1) % uhpd->peers->number;
                peer = &uhpd->peers->peer[current];
                if (current == start_index) {
                    break;
                }
            }
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "upstream_hash: start peer:%ui chose peer:%ui",
                       start_index, current);
#endif
        uhpd->peer_index = current;
    }
}

#if (NGX_HTTP_SSL)
static ngx_int_t
ngx_http_upstream_set_hash_peer_session(ngx_peer_connection_t *pc, void *data) {
    ngx_http_upstream_hash_peer_data_t  *uhpd = data;

    ngx_int_t                       rc;
    ngx_ssl_session_t              *ssl_session;
    ngx_http_upstream_hash_peer_t  *peer;
    ngx_uint_t                           current;

    current = uhpd->hash % uhpd->peers->number;

    peer = &uhpd->peers->peer[current];

    /* TODO: threads only mutex */
    /* ngx_lock_mutex(rrp->peers->mutex); */

    ssl_session = peer->ssl_session;

    rc = ngx_ssl_set_session(pc->connection, ssl_session);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "set session: %p:%d",
                   ssl_session, ssl_session ? ssl_session->references : 0);

    /* ngx_unlock_mutex(rrp->peers->mutex); */

    return rc;
}

static void
ngx_http_upstream_save_hash_peer_session(ngx_peer_connection_t *pc, void *data) {
    ngx_http_upstream_hash_peer_data_t *uhpd = data;
    ngx_ssl_session_t            *old_ssl_session, *ssl_session;
    ngx_http_upstream_hash_peer_t  *peer;
    ngx_uint_t                           current;

    ssl_session = ngx_ssl_get_session(pc->connection);

    if (ssl_session == NULL) {
        return;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "save session: %p:%d", ssl_session, ssl_session->references);

    current = uhpd->hash % uhpd->peers->number;

    peer = &uhpd->peers->peer[current];

    /* TODO: threads only mutex */
    /* ngx_lock_mutex(rrp->peers->mutex); */

    old_ssl_session = peer->ssl_session;
    peer->ssl_session = ssl_session;

    /* ngx_unlock_mutex(rrp->peers->mutex); */

    if (old_ssl_session) {

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "old session: %p:%d",
                       old_ssl_session, old_ssl_session->references);

        /* TODO: may block */

        ngx_ssl_free_session(old_ssl_session);
    }
}
#endif

static void ngx_http_upstream_hash_next_peer(ngx_http_upstream_hash_peer_data_t *uhpd,
        ngx_uint_t *tries, ngx_log_t *log) {

    ngx_uint_t current;
    current = uhpd->hash % uhpd->peers->number;
    //  Loop while there is a try left, we're on one we haven't tried
    while ((*tries)--
           && (uhpd->tried[ngx_bitvector_index(current)] & ngx_bitvector_bit(current))) {

       uhpd->current_key.len = ngx_sprintf(uhpd->current_key.data, "%d%V",
           ++uhpd->try_i, &uhpd->original_key) - uhpd->current_key.data;
       uhpd->hash += ngx_http_upstream_hash_crc32(uhpd->current_key.data,
           uhpd->current_key.len);
       current = uhpd->hash % uhpd->peers->number;
       ngx_log_debug2(NGX_LOG_DEBUG_HTTP, log, 0,
           "upstream_hash: hashed \"%V\" to %ui", &uhpd->current_key, current);
   }
   uhpd->peer_index = current;
}

/* bit-shift, bit-mask, and non-zero requirement are for libmemcache compatibility */
static ngx_uint_t
ngx_http_upstream_hash_crc32(u_char *keydata, size_t keylen)
{
    ngx_uint_t crc32 = (ngx_crc32_short(keydata, keylen) >> 16) & 0x7fff;
    return crc32 ? crc32 : 1;
}

static char *
ngx_http_upstream_hash(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_http_script_compile_t      sc;
    ngx_str_t                     *value;
    ngx_array_t                   *vars_lengths, *vars_values;

    value = cf->args->elts;

    ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));

    vars_lengths = NULL;
    vars_values = NULL;

    sc.cf = cf;
    sc.source = &value[1];
    sc.lengths = &vars_lengths;
    sc.values = &vars_values;
    sc.complete_lengths = 1;
    sc.complete_values = 1;

    if (ngx_http_script_compile(&sc) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    uscf->peer.init_upstream = ngx_http_upstream_init_hash;

    uscf->flags = NGX_HTTP_UPSTREAM_CREATE | NGX_HTTP_UPSTREAM_DOWN;

    uscf->values = vars_values->elts;
    uscf->lengths = vars_lengths->elts;

    return NGX_CONF_OK;
}

static char *
ngx_http_upstream_hash_again(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_int_t n;

    ngx_str_t *value;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    value = cf->args->elts;

    n = ngx_atoi(value[1].data, value[1].len);

    if (n == NGX_ERROR || n < 0) {
        return "invalid number";
    }

    uscf->retries = n;

    return NGX_CONF_OK;
}
