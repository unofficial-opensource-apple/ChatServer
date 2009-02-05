/*
 * jabberd - Jabber Open Source Server
 * Copyright (c) 2002 Jeremie Miller, Thomas Muldowney,
 *                    Ryan Eatmon, Robert Norris
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA02111-1307USA
 */

#ifdef HAVE_CONFIG_H
#   include <config.h>
#endif

#include "mio/mio.h"
#include "sx/sx.h"
#include "sx/ssl.h"
#include "sx/sasl.h"

#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#include <pwd.h>

/* forward decl */
typedef struct s2s_st       *s2s_t;
typedef struct pkt_st       *pkt_t;
typedef struct conn_st      *conn_t;
typedef struct dnscache_st  *dnscache_t;

struct s2s_st {
    /** our id (hostname) with the router */
    char                *id;

    /** how to connect to the router */
    char                *router_ip;
    int                 router_port;
    char                *router_user;
    char                *router_pass;
    char                *router_pemfile;
    int                 router_default;

    /** mio context */
    mio_t               mio;

    /** sx environment */
    sx_env_t            sx_env;
    sx_plugin_t         sx_ssl;
    sx_plugin_t         sx_sasl;
    sx_plugin_t         sx_db;

    /** router's conn */
    sx_t                router;
    int                 fd;

    /** listening sockets */
    int                 server_fd;

    /** config */
    config_t            config;

    /** logging */
    log_t               log;

    /** log data */
    log_type_t          log_type;
    char                *log_facility;
    char                *log_ident;

    /** connect retry */
    int                 retry_init;
    int                 retry_lost;
    int                 retry_sleep;
    int                 retry_left;

    /** ip/port to listen on */
    char                *local_ip;
    int                 local_port;

    /** id of resolver */
    char                *local_resolver;

    /** dialback secret */
    char                *local_secret;

    /** pemfile for peer connections */
    char                *local_pemfile;

    /** time checks */
    int                 check_interval;
    int                 check_queue;
    int                 check_invalid;
    int                 check_keepalive;
    int                 check_idle;

	/** Apple security options */
	int					require_tls;
	int					enable_whitelist;
	char                **whitelist_domains;
	int					n_whitelist_domains;

    time_t              last_queue_check;
    time_t              last_invalid_check;

    time_t              next_check;

    /** stringprep cache */
    prep_cache_t        pc;

    /** list of sx_t on the way out */
    jqueue_t            dead;

    /** this is true if we've connected to the router at least once */
    int                 started;

    /** true if we're bound in the router */
    int                 online;

    /** queues of packets waiting to go out (key is dest domain) */
    xht                 outq;

    /** outgoing conns (key is ip/port) */
    xht                 out;

    /** incoming conns (key is stream id) */
    xht                 in;

    /** incoming conns prior to stream initiation (key is ip/port) */
    xht                 in_accept;

    /** dns resolution cache */
    xht                 dnscache;
};

struct pkt_st {
    nad_t               nad;

    jid_t               from;
    jid_t               to;

    int                 db;

    char                ip[INET6_ADDRSTRLEN];
    int                 port;
};

typedef enum {
    conn_NONE,
    conn_INPROGRESS,
    conn_VALID,
    conn_INVALID
} conn_state_t;

struct conn_st {
    s2s_t               s2s;

    char                *key;

    sx_t                s;
    int                 fd;

    char                ip[INET6_ADDRSTRLEN];
    int                 port;

    /** states of outgoing dialbacks (key is local/remote) */
    xht                 states;

    /** time of the last state change (key is local/remote) */
    xht                 states_time;

    /** routes that this conn handles (key is local/remote) */
    xht                 routes;

    time_t              init_time;

    int                 online;
    
    /** number and last timestamp of outstanding db:verify requests */
    int                 verify;
    time_t              last_verify;

    /** timestamps for idle timeouts */
    time_t              last_activity;
    time_t              last_packet;
};

/** one item in the dns resolution cache */
struct dnscache_st {
    /** the name proper */
    char                name[1024];

    /** ip and port that the name resolves to */
    char                ip[INET6_ADDRSTRLEN];
    int                 port;

    /** time that this entry expires */
    time_t              expiry;

    time_t              init_time;

    /** set when we're waiting for a resolve response */
    int                 pending;
};

extern sig_atomic_t s2s_lost_router;

int             s2s_router_mio_callback(mio_t m, mio_action_t a, int fd, void *data, void *arg);
int             s2s_router_sx_callback(sx_t s, sx_event_t e, void *data, void *arg);

char            *s2s_route_key(pool p, char *local, char *remote);
char            *s2s_db_key(pool p, char *secret, char *remote, char *id);

void            out_packet(s2s_t s2s, pkt_t pkt);
void            out_resolve(s2s_t s2s, nad_t nad);
void            out_dialback(s2s_t s2s, pkt_t pkt);
int             out_bounce_queue(s2s_t s2s, const char *domain, int err);
int             out_bounce_conn_queues(conn_t out, int err);

int             in_mio_callback(mio_t m, mio_action_t a, int fd, void *data, void *arg);

/* sx flag for outgoing dialback streams */
#define S2S_DB_HEADER   (1<<10)

/* max length of FQDN for whitelist matching */
#define MAX_DOMAIN_LEN	1023

int             s2s_db_init(sx_env_t env, sx_plugin_t p, va_list args);

/* union for xhash_iter_get to comply with strict-alias rules for gcc3 */
union xhashv
{
  void **val;
  char **char_val;
  conn_t *conn_val;
  conn_state_t *state_val;
  jqueue_t *jq_val;
  dnscache_t *dns_val;
};
