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

#include "s2s.h"

#ifdef HAVE_IDN
#include <stringprep.h>
#endif

static sig_atomic_t s2s_shutdown = 0;
sig_atomic_t s2s_lost_router = 0;
static sig_atomic_t s2s_logrotate = 0;

static void _s2s_signal(int signum) {
    s2s_shutdown = 1;
    s2s_lost_router = 0;
}

static void _s2s_signal_hup(int signum) {
    s2s_logrotate = 1;
}

/** store the process id */
static void _s2s_pidfile(s2s_t s2s) {
    char *pidfile;
    FILE *f;
    pid_t pid;

    pidfile = config_get_one(s2s->config, "pidfile", 0);
    if(pidfile == NULL)
        return;

    pid = getpid();

    if((f = fopen(pidfile, "w+")) == NULL) {
        log_write(s2s->log, LOG_ERR, "couldn't open %s for writing: %s", pidfile, strerror(errno));
        return;
    }

    if(fprintf(f, "%d", pid) < 0) {
        log_write(s2s->log, LOG_ERR, "couldn't write to %s: %s", pidfile, strerror(errno));
        return;
    }

    fclose(f);

    log_write(s2s->log, LOG_INFO, "process id is %d, written to %s", pid, pidfile);
}

/** pull values out of the config file */
static void _s2s_config_expand(s2s_t s2s) {
    char *str, secret[41];
    int i, r;
    config_elem_t elem;

    s2s->id = config_get_one(s2s->config, "id", 0);
    if(s2s->id == NULL)
        s2s->id = "s2s";

    s2s->router_ip = config_get_one(s2s->config, "router.ip", 0);
    if(s2s->router_ip == NULL)
        s2s->router_ip = "127.0.0.1";

    s2s->router_port = j_atoi(config_get_one(s2s->config, "router.port", 0), 5347);

    s2s->router_user = config_get_one(s2s->config, "router.user", 0);
    if(s2s->router_user == NULL)
        s2s->router_user = "jabberd";
    s2s->router_pass = config_get_one(s2s->config, "router.pass", 0);
    if(s2s->router_pass == NULL)
        s2s->router_pass = "secret";

    s2s->router_pemfile = config_get_one(s2s->config, "router.pemfile", 0);

    s2s->retry_init = j_atoi(config_get_one(s2s->config, "router.retry.init", 0), 3);
    s2s->retry_lost = j_atoi(config_get_one(s2s->config, "router.retry.lost", 0), 3);
    if((s2s->retry_sleep = j_atoi(config_get_one(s2s->config, "router.retry.sleep", 0), 2)) < 1)
        s2s->retry_sleep = 1;

    s2s->router_default = config_count(s2s->config, "router.non-default") ? 0 : 1;

    s2s->log_type = log_STDOUT;
    if(config_get(s2s->config, "log") != NULL) {
        if((str = config_get_attr(s2s->config, "log", 0, "type")) != NULL) {
            if(strcmp(str, "file") == 0)
                s2s->log_type = log_FILE;
            else if(strcmp(str, "syslog") == 0)
                s2s->log_type = log_SYSLOG;
        }
    }

    if(s2s->log_type == log_SYSLOG) {
        s2s->log_facility = config_get_one(s2s->config, "log.facility", 0);
        s2s->log_ident = config_get_one(s2s->config, "log.ident", 0);
        if(s2s->log_ident == NULL)
            s2s->log_ident = "jabberd/s2s";
    } else if(s2s->log_type == log_FILE)
        s2s->log_ident = config_get_one(s2s->config, "log.file", 0);

    s2s->local_ip = config_get_one(s2s->config, "local.ip", 0);
    if(s2s->local_ip == NULL)
        s2s->local_ip = "0.0.0.0";

    s2s->local_port = j_atoi(config_get_one(s2s->config, "local.port", 0), 0);

    s2s->local_resolver = config_get_one(s2s->config, "local.resolver", 0);
    if(s2s->local_resolver == NULL)
        s2s->local_resolver = "resolver";

    if(config_get(s2s->config, "local.secret") != NULL)
        s2s->local_secret = strdup(config_get_one(s2s->config, "local.secret", 0));
    else {
        for(i = 0; i < 40; i++) {
            r = (int) (36.0 * rand() / RAND_MAX);
            secret[i] = (r >= 0 && r <= 9) ? (r + 48) : (r + 87);
        }
        secret[40] = '\0';

        s2s->local_secret = strdup(secret);
    }

    if(s2s->local_secret == NULL)
        s2s->local_secret = "secret";

    s2s->local_pemfile = config_get_one(s2s->config, "local.pemfile", 0);
    if (s2s->local_pemfile != NULL)
 		log_debug(ZONE,"loaded local pemfile for peer s2s connections");

	s2s->require_tls = j_atoi(config_get_one(s2s->config, "security.require_tls", 0), 0);
	s2s->enable_whitelist = j_atoi(config_get_one(s2s->config, "security.enable_whitelist", 0), 0);
	if((elem = config_get(s2s->config, "security.whitelist_domain")) != NULL) {
		_s2s_populate_whitelist_domains(s2s, elem->values, elem->nvalues);
	}

    s2s->check_interval = j_atoi(config_get_one(s2s->config, "check.interval", 0), 60);
    s2s->check_queue = j_atoi(config_get_one(s2s->config, "check.queue", 0), 60);
    s2s->check_keepalive = j_atoi(config_get_one(s2s->config, "check.keepalive", 0), 0);
    s2s->check_idle = j_atoi(config_get_one(s2s->config, "check.idle", 0), 86400);

}

static int _s2s_router_connect(s2s_t s2s) {
    log_write(s2s->log, LOG_NOTICE, "attempting connection to router at %s, port=%d", s2s->router_ip, s2s->router_port);

    s2s->fd = mio_connect(s2s->mio, s2s->router_port, s2s->router_ip, s2s_router_mio_callback, (void *) s2s);
    if(s2s->fd < 0) {
        if(errno == ECONNREFUSED)
            s2s_lost_router = 1;
        log_write(s2s->log, LOG_NOTICE, "connection attempt to router failed: %s (%d)", strerror(errno), errno);
        return 1;
    }

    s2s->router = sx_new(s2s->sx_env, s2s->fd, s2s_router_sx_callback, (void *) s2s);
    sx_client_init(s2s->router, 0, NULL, NULL, NULL, "1.0");

    return 0;
}

int _s2s_check_conn_routes(s2s_t s2s, conn_t conn, const char *direction)
{
  char *rkey;
  conn_state_t state;
  time_t now, dialback_time;
  union xhashv xhv;

  now = time(NULL);

  if(xhash_iter_first(conn->states))
     do {
           xhv.state_val = &state;
           xhash_iter_get(conn->states, (const char **) &rkey, xhv.val);
           if (state == conn_INPROGRESS) {
              dialback_time = (time_t) xhash_get(conn->states_time, rkey);

              if(now > dialback_time + s2s->check_queue) {
                 log_write(s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] dialback for %s route '%s' timed out", conn->fd, conn->ip, conn->port, direction, rkey);

                 xhash_zap(conn->states, rkey);
                 xhash_zap(conn->states_time, rkey);

                 /* stream error */
                 sx_error(conn->s, stream_err_CONNECTION_TIMEOUT, "dialback timed out");

                 /* close connection as per XMPP/RFC3920 */
                 sx_close(conn->s);

                 /* indicate that we closed the connection */
                 return 0;
              }
           }
     } while(xhash_iter_next(conn->states));

  /* all ok */
  return 1;
}

static void _s2s_time_checks(s2s_t s2s) {
    conn_t conn;
    time_t now;
    char *domain, ipport[INET6_ADDRSTRLEN + 17], *key;
    jqueue_t q;
    dnscache_t dns;
    union xhashv xhv;

    now = time(NULL);

    /* queue expiry */
    if(s2s->check_queue > 0) {
        if(xhash_iter_first(s2s->outq))
            do {
                xhv.jq_val = &q;
                xhash_iter_get(s2s->outq, (const char **) &domain, xhv.val);

                log_debug(ZONE, "running time checks for %s", domain);

                /* dns lookup timeout check first */
                dns = xhash_get(s2s->dnscache, domain);
                if(dns == NULL)
                    continue;

                if(dns->pending) {
                    log_debug(ZONE, "dns lookup pending for %s", domain);
                    if(now > dns->init_time + s2s->check_queue) {
                        log_write(s2s->log, LOG_NOTICE, "dns lookup for %s timed out", domain);

                        /* bounce queue */
                        out_bounce_queue(s2s, domain, stanza_err_REMOTE_SERVER_NOT_FOUND);

                        /* expire pending dns entry */
                        xhash_zap(s2s->dnscache, dns->name);
                        free(dns);
                    }

                    continue;
                }

                /* generate the ip/port pair */
                snprintf(ipport, INET6_ADDRSTRLEN + 16, "%s/%d", dns->ip, dns->port);

                /* get the conn */
                conn = xhash_get(s2s->out, ipport);
                if(conn == NULL) {
                    if(jqueue_size(q) > 0) {
                       /* no pending conn? perhaps it failed? */
                       log_debug(ZONE, "no pending connection for %s, bouncing %i packets in queue", domain, jqueue_size(q));

                       /* bounce queue */
                       out_bounce_queue(s2s, domain, stanza_err_REMOTE_SERVER_TIMEOUT);
                    }

                    continue;
                }

                /* connect timeout check */
                if(!conn->online && now > conn->init_time + s2s->check_queue) {
                    log_write(s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] connection to %s timed out", conn->fd, conn->ip, conn->port, domain);

                    /* bounce queue */
                    out_bounce_queue(s2s, domain, stanza_err_REMOTE_SERVER_TIMEOUT);

                    /* close connection as per XMPP/RFC3920 */
                    sx_close(conn->s);

                }
            } while(xhash_iter_next(s2s->outq));
    }

    /* expiry of connected routes in conn_INPROGRESS state */
    if(s2s->check_queue > 0) {

        /* outgoing connections */
        if(xhash_iter_first(s2s->out))
            do {
                xhv.conn_val = &conn;
                xhash_iter_get(s2s->out, (const char **) &key, xhv.val);
                log_debug(ZONE, "checking dialback state for outgoing conn %s", key);
                if (_s2s_check_conn_routes(s2s, conn, "outgoing")) {
                    log_debug(ZONE, "checking pending verify requests for outgoing conn %s", key);
                    if (conn->verify > 0 && now > conn->last_verify + s2s->check_queue) {
                        log_write(s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] dialback verify request timed out", conn->fd, conn->ip, conn->port);
                        sx_error(conn->s, stream_err_CONNECTION_TIMEOUT, "dialback verify request timed out");
                        sx_close(conn->s);
                    }
                }
            } while(xhash_iter_next(s2s->out));

        /* incoming open streams */
        if(xhash_iter_first(s2s->in))
            do {
                xhv.conn_val = &conn;
                xhash_iter_get(s2s->in, (const char **) &key, xhv.val);

                log_debug(ZONE, "checking dialback state for incoming conn %s", key);
                if (_s2s_check_conn_routes(s2s, conn, "incoming"))
                    /* if the connection is still valid, check that dialbacks have been initiated */
                    if(!xhash_count(conn->states) && now > conn->init_time + s2s->check_queue) {
                        log_write(s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] no dialback started", conn->fd, conn->ip, conn->port); 
                        sx_error(conn->s, stream_err_CONNECTION_TIMEOUT, "no dialback initiated");
                        sx_close(conn->s);
                    }
            } while(xhash_iter_next(s2s->in));

        /* incoming open connections (not yet streams) */
        if(xhash_iter_first(s2s->in_accept))
            do {
                xhv.conn_val = &conn;
                xhash_iter_get(s2s->in_accept, (const char **) &key, xhv.val);

                log_debug(ZONE, "checking stream connection state for incoming conn %i", conn->fd);
                if(!conn->online && now > conn->init_time + s2s->check_queue) {
                    log_write(s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] stream initiation timed out", conn->fd, conn->ip, conn->port);
                    sx_close(conn->s);
                }
            } while(xhash_iter_next(s2s->in_accept));

    }

    /* keepalives */
    if(xhash_iter_first(s2s->out))
        do {
            xhv.conn_val = &conn;
            xhash_iter_get(s2s->out, NULL, xhv.val);

            if(s2s->check_keepalive > 0 && conn->last_activity > 0 && now > conn->last_activity + s2s->check_keepalive && conn->s->state >= state_STREAM) {
                log_debug(ZONE, "sending keepalive for %d", conn->fd);

                sx_raw_write(conn->s, " ", 1);
                
                mio_write(s2s->mio, conn->fd);
            }
        } while(xhash_iter_next(s2s->out));

    /* idle timeouts - disconnect connections through which no packets have been sent for <idle> seconds */
    if(s2s->check_idle > 0) {

       /* outgoing connections */
        if(xhash_iter_first(s2s->out))
            do {
                xhv.conn_val = &conn;
                xhash_iter_get(s2s->out, (const char **) &key, xhv.val);
                log_debug(ZONE, "checking idle state for %s", key);
                if (conn->last_packet > 0 && now > conn->last_packet + s2s->check_idle && conn->s->state >= state_STREAM) {
                    log_write(s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] idle timeout", conn->fd, conn->ip, conn->port);
                    sx_close(conn->s);
                }
            } while(xhash_iter_next(s2s->out));

       /* incoming connections */
        if(xhash_iter_first(s2s->in))
            do {
                xhv.conn_val = &conn;
                xhash_iter_get(s2s->in, (const char **) &key, xhv.val);
                log_debug(ZONE, "checking idle state for %s", key);
                if (conn->last_packet > 0 && now > conn->last_packet + s2s->check_idle && conn->s->state >= state_STREAM) {
                    log_write(s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] idle timeout", conn->fd, conn->ip, conn->port);
                    sx_close(conn->s);
                }
            } while(xhash_iter_next(s2s->in));

    }

    return;
}

/* Populate the whitelist_domains array with the config file values */
int _s2s_populate_whitelist_domains(s2s_t s2s, char **values, int nvalues) {
	int i, j;
	int elem_len;
	//log_write(s2s->log, LOG_NOTICE, "calling malloc 1, nvalues = %i", nvalues);
    s2s->whitelist_domains = (char **)malloc(sizeof(char*) * (nvalues));
	memset(s2s->whitelist_domains, 0, (sizeof(char *) * (nvalues)));	
    for (i = 0, j = 0; i < nvalues; i++) {
		elem_len = strlen(values[i]);
        if (elem_len > MAX_DOMAIN_LEN) {
			log_write(s2s->log, LOG_NOTICE, "whitelist domain element is too large, skipping");
			continue;
        }
        if (elem_len == 0) {
			log_write(s2s->log, LOG_NOTICE, "whitelist domain element is blank, skipping");
            continue;
        }
		//log_write(s2s->log, LOG_NOTICE, "calling malloc 2, elem is \"%s\", elem_len = %i", values[i], elem_len);
        s2s->whitelist_domains[j] = (char *) malloc(sizeof(char) * (elem_len+1));
		memset(s2s->whitelist_domains[j], 0, (sizeof(char) * (elem_len+1)));
        strncpy(s2s->whitelist_domains[j], values[i], elem_len);
		s2s->whitelist_domains[j][elem_len+1] = '\0';
        log_debug(ZONE, "s2s whitelist domain read from file: %s\n", s2s->whitelist_domains[j]);
		j++;
    }

	s2s->n_whitelist_domains = j;
	log_write(s2s->log, LOG_NOTICE, "n_whitelist_domains = %i", s2s->n_whitelist_domains);
    return 0;
}


/* Compare a domain with whitelist values.
	The whitelist values may be FQDN or domain only (with no prepended hostname).
	returns 1 on match, 0 on failure to match
*/
int _s2s_domain_in_whitelist(s2s_t s2s, char *in_domain) {
	char *wl_domain = NULL;
	int segcount = 0;
	int dotcount;
	char **segments = NULL;
	char **dst = NULL;
	char *seg_tmp = NULL;	
	int seg_tmp_len;
	char matchstr[MAX_DOMAIN_LEN + 1];
	int domain_index;
	int x, i;
	int wl_index;
	int wl_len;
	int matchstr_len;
	char domain[strlen(in_domain)+1];
	char *domain_ptr = &domain[0];
	int domain_len;

	strlcpy((char *)&domain, in_domain, sizeof(domain));
	domain_len = strlen((const char *)&domain);

	if (domain_len <= 0) {
		log_write(s2s->log, LOG_NOTICE, "_s2s_domain_in_whitelist: in_domain is empty");
		return 0;
	}

	if (domain_len > MAX_DOMAIN_LEN) {
		log_write(s2s->log, LOG_NOTICE, "_s2s_domain_in_whitelist: in_domain is longer than %s chars", MAX_DOMAIN_LEN);
		return 0;
	}

	// first try matching the FQDN with whitelist domains
	if (s2s->n_whitelist_domains <= 0)
		return 0;

	for (wl_index =0; wl_index < s2s->n_whitelist_domains; wl_index++) {
		wl_len = strlen(s2s->whitelist_domains[wl_index]);
		if (!strncmp((const char *)&domain, s2s->whitelist_domains[wl_index], (domain_len > wl_len) ? domain_len : wl_len)) {
			log_debug(ZONE, "domain \"%s\" matches whitelist entry", &domain);
			return 1;
		}
		else {
			//log_debug(ZONE, "domain: %s (len %d) does not match whitelist_domains[%d]: %s (len %d)", &domain, strlen((const char *)&domain), wl_index, s2s->whitelist_domains[wl_index], strlen(s2s->whitelist_domains[wl_index]));
		}
	}

	// break domain into segments for domain-only comparision
	for (dotcount = 0, x = 0; domain[x] != '\0'; x++) {
		if (domain[x] == '.')
			dotcount++;
	}
		
	segments = (char **)malloc(sizeof(char*) * (dotcount + 1));
	if (segments == NULL) {
		log_write(s2s->log, LOG_ERR, "_s2s_domain_in_whitelist: malloc() error");
		return 0;
	}
	memset((char **)segments, 0, (sizeof(char*) * (dotcount + 1)));

	do {
		if (segcount > (dotcount+1)) {
			log_write(s2s->log, LOG_ERR, "_s2s_domain_in_whitelist: did not malloc enough room for domain segments; should never get here");
			if (seg_tmp != NULL) {
				free(seg_tmp);
				seg_tmp = NULL;
			}
			for (x = 0; x < segcount; x++) {
				free(segments[x]);
				segments[x] = NULL;
			}
			free(segments);
			segments = NULL;
			return 0;
		}
		seg_tmp = strsep(&domain_ptr, ".");
		if (seg_tmp == NULL) {
			break;
		}

		seg_tmp_len = strlen(seg_tmp);
		if (seg_tmp_len > MAX_DOMAIN_LEN) {
			log_write(s2s->log, LOG_NOTICE, "_s2s_domain_in_whitelist: domain contains a segment greater than %s chars", MAX_DOMAIN_LEN);
			if (seg_tmp != NULL) {
				free(seg_tmp);
				seg_tmp = NULL;
			}
			for (x = 0; x < segcount; x++) {
				free(segments[x]);
				segments[x] = NULL;
			}   
			free(segments);
			segments = NULL;
			return 0;
		}
		dst = &segments[segcount];
		*dst = (char *)malloc(seg_tmp_len + 1);
		if (*dst != NULL) {
			strlcpy(*dst, seg_tmp, seg_tmp_len + 1);
		} else { 
			if (seg_tmp != NULL) {
				free(seg_tmp);
				seg_tmp = NULL;
			}
			for (x = 0; x < segcount; x++) {
				free(segments[x]);
				segments[x] = NULL;
			}   
			free(segments);
			segments = NULL;
			log_write(s2s->log, LOG_ERR, "_s2s_domain_in_whitelist: malloc() error");
			return 0;
		}
		segcount++;
    } while (seg_tmp != NULL);

	if (segcount > 1) {
		for (domain_index = segcount-2; domain_index > 0; domain_index--) {
			matchstr[0] = '\0';
			for (i = domain_index; i < segcount; i++) {
				if (i > domain_index)
					strlcat((char *)&matchstr, ".", sizeof(matchstr));
				strlcat((char *)&matchstr, (char *)segments[i], sizeof(matchstr));
			}
			for (wl_index = 0; wl_index < s2s->n_whitelist_domains; wl_index++) {
				wl_len = strlen(s2s->whitelist_domains[wl_index]);
				matchstr_len = strlen((const char *)&matchstr);
				if (!strncmp((const char *)&matchstr, s2s->whitelist_domains[wl_index], (wl_len > matchstr_len ? wl_len : matchstr_len))) {
					log_debug(ZONE, "matchstr \"%s\" matches whitelist entry", &matchstr);
					for (x = 0; x < segcount; x++) {
						free(segments[x]);
						segments[x] = NULL;
					}   
					free(segments);
					segments = NULL;
					return 1;
				} 
				else { 
					//log_debug(ZONE, "matchstr: %s (len %d) does not match whitelist_domains[%d]: %s (len %d)", &matchstr, strlen((const char *)&matchstr), wl_index, s2s->whitelist_domains[wl_index], strlen(s2s->whitelist_domains[wl_index]));
				}
			}
		}
    }
	for (x = 0; x < segcount; x++) {
		free(segments[x]);
		segments[x] = NULL;
	}   
	free(segments);
	segments = NULL;

	return 0;	
}

int main(int argc, char **argv) {
    s2s_t s2s;
    char *config_file;
    int optchar;
    conn_t conn;
    jqueue_t q;
    dnscache_t dns;
    union xhashv xhv;
	int newuid, newgid;
	struct passwd *p;

#ifdef POOL_DEBUG
    time_t pool_time = 0;
#endif

#ifdef JABBER_USER
    p = getpwnam(JABBER_USER);
    if (p == NULL) {
        printf("Error: could not find user %s\n", JABBER_USER);
        return 1;
    }
	newuid = p->pw_uid;
	newgid = p->pw_gid;
	memset(p, 0, sizeof(struct passwd));

    if (initgroups(JABBER_USER, newgid)) {
        printf("cannot initialize groups for user %s: %s\n", JABBER_USER, strerror(errno));
        return 1;
    }

    if (setgid(newgid)) {
        printf("cannot setgid: %s\n", strerror(errno));
        return 1;
    }

    if (seteuid(newuid)) {
        printf("cannot seteuid: %s\n", strerror(errno));
        return 1;
    }
#else
    printf("No user is defined for setuid/setgid, continuing\n");
#endif


#ifdef HAVE_UMASK
    umask((mode_t) 0027);
#endif

    srand(time(NULL));

#ifdef HAVE_WINSOCK2_H
/* get winsock running */
	{
		WORD wVersionRequested;
		WSADATA wsaData;
		int err;
		
		wVersionRequested = MAKEWORD( 2, 2 );
		
		err = WSAStartup( wVersionRequested, &wsaData );
		if ( err != 0 ) {
            /* !!! tell user that we couldn't find a usable winsock dll */
			return 0;
		}
	}
#endif

    jabber_signal(SIGINT, _s2s_signal);
    jabber_signal(SIGTERM, _s2s_signal);
#ifdef SIGHUP
    jabber_signal(SIGHUP, _s2s_signal_hup);
#endif
#ifdef SIGPIPE
    jabber_signal(SIGPIPE, SIG_IGN);
#endif

    s2s = (s2s_t) malloc(sizeof(struct s2s_st));
    memset(s2s, 0, sizeof(struct s2s_st));

    /* load our config */
    s2s->config = config_new();

    config_file = CONFIG_DIR "/s2s.xml";

    /* cmdline parsing */
    while((optchar = getopt(argc, argv, "Dc:h?")) >= 0)
    {
        switch(optchar)
        {
            case 'c':
                config_file = optarg;
                break;
            case 'D':
#ifdef DEBUG
                set_debug_flag(1);
#else
                printf("WARN: Debugging not enabled.  Ignoring -D.\n");
#endif
                break;
            case 'h': case '?': default:
                fputs(
                    "s2s - jabberd server-to-server connector (" VERSION ")\n"
                    "Usage: s2s <options>\n"
                    "Options are:\n"
                    "   -c <config>     config file to use [default: " CONFIG_DIR "/s2s.xml]\n"
#ifdef DEBUG
                    "   -D              Show debug output\n"
#endif
                    ,
                    stdout);
                config_free(s2s->config);
                free(s2s);
                return 1;
        }
    }

    if(config_load(s2s->config, config_file) != 0) {
        fputs("s2s: couldn't load config, aborting\n", stderr);
        config_free(s2s->config);
        free(s2s);
        return 2;
    }

    s2s->log = log_new(s2s->log_type, s2s->log_ident, s2s->log_facility);
    _s2s_config_expand(s2s);

    log_write(s2s->log, LOG_NOTICE, "starting up (interval=%i, queue=%i, keepalive=%i, idle=%i)", s2s->check_interval, s2s->check_queue, s2s->check_keepalive, s2s->check_idle);

    _s2s_pidfile(s2s);

    s2s->outq = xhash_new(401);
    s2s->out = xhash_new(401);
    s2s->in = xhash_new(401);
    s2s->in_accept = xhash_new(401);
    s2s->dnscache = xhash_new(401);

    s2s->pc = prep_cache_new();

    s2s->dead = jqueue_new();

    s2s->sx_env = sx_env_new();

#ifdef HAVE_SSL
    /* get the ssl context up and running */
#ifdef JABBER_USER
    if (seteuid(0)) {
		log_write(s2s->log, LOG_ERR, "cannot seteuid to root: %s", strerror(errno));
        return 1;
    }
#else
	log_write(s2s->log, LOG_NOTICE, "No user is defined for setuid/setgid, continuing");
#endif // JABBER_USER

    if(s2s->local_pemfile != NULL) {
        s2s->sx_ssl = sx_env_plugin(s2s->sx_env, sx_ssl_init, s2s->local_pemfile, NULL);
        if(s2s->sx_ssl == NULL) {
            log_write(s2s->log, LOG_ERR, "failed to load local SSL pemfile, SSL will not be available to peers");
            s2s->local_pemfile = NULL;
        } else
            log_debug(ZONE, "loaded pemfile for SSL connections to peers");
    }

    /* try and get something online, so at least we can encrypt to the router */
    if(s2s->sx_ssl == NULL && s2s->router_pemfile != NULL) {
        s2s->sx_ssl = sx_env_plugin(s2s->sx_env, sx_ssl_init, s2s->router_pemfile, NULL);
        if(s2s->sx_ssl == NULL) {
            log_write(s2s->log, LOG_ERR, "failed to load router SSL pemfile, channel to router will not be SSL encrypted");
            s2s->router_pemfile = NULL;
        }
    }
#endif // HAVE_SSL
#ifdef JABBER_USER
    if (setuid(newuid)) {
        log_write(s2s->log, LOG_ERR, "cannot setuid(%d): %s", newuid, strerror(errno));
        return 1;
    }
#else
    log_write(s2s->log, LOG_NOTICE, "No user is defined for setuid/setgid, continuing");
#endif // JABBER_USER

    /* get sasl online */
    s2s->sx_sasl = sx_env_plugin(s2s->sx_env, sx_sasl_init, "xmpp", SASL_SEC_NOANONYMOUS | SASL_SEC_NOPLAINTEXT, NULL, NULL, 0);
    if(s2s->sx_sasl == NULL) {
        log_write(s2s->log, LOG_ERR, "failed to initialise SASL context, aborting");
        exit(1);
    }

    s2s->sx_db = sx_env_plugin(s2s->sx_env, s2s_db_init);

    s2s->mio = mio_new(1024);

    s2s->retry_left = s2s->retry_init;
    _s2s_router_connect(s2s);

    while(!s2s_shutdown) {
        mio_run(s2s->mio, 5);

        if(s2s_logrotate) {
            log_write(s2s->log, LOG_NOTICE, "reopening log ...");
            log_free(s2s->log);
            s2s->log = log_new(s2s->log_type, s2s->log_ident, s2s->log_facility);
            log_write(s2s->log, LOG_NOTICE, "log started");

            s2s_logrotate = 0;
        }

        if(s2s_lost_router) {
            if(s2s->retry_left < 0) {
                log_write(s2s->log, LOG_NOTICE, "attempting reconnect");
                sleep(s2s->retry_sleep);
                s2s_lost_router = 0;
                _s2s_router_connect(s2s);
            }

            else if(s2s->retry_left == 0) {
                s2s_shutdown = 1;
            }

            else {
                log_write(s2s->log, LOG_NOTICE, "attempting reconnect (%d left)", s2s->retry_left);
                s2s->retry_left--;
                sleep(s2s->retry_sleep);
                s2s_lost_router = 0;
                _s2s_router_connect(s2s);
            }
        }
            
        /* cleanup dead sx_ts */
        while(jqueue_size(s2s->dead) > 0)
            sx_free((sx_t) jqueue_pull(s2s->dead));

        /* time checks */
        if(s2s->check_interval > 0 && time(NULL) >= s2s->next_check) {
            log_debug(ZONE, "running time checks");

            _s2s_time_checks(s2s);

            s2s->next_check = time(NULL) + s2s->check_interval;
            log_debug(ZONE, "next time check at %d", s2s->next_check);
        }

#ifdef POOL_DEBUG
        if(time(NULL) > pool_time + 60) {
            pool_stat(1);
            pool_time = time(NULL);
        }
#endif
    }

    log_write(s2s->log, LOG_NOTICE, "shutting down");

    /* close active streams gracefully  */
    xhv.conn_val = &conn;
    if(xhash_iter_first(s2s->out))
        do {
            xhash_iter_get(s2s->out, NULL, xhv.val);
            sx_error(conn->s, stream_err_SYSTEM_SHUTDOWN, "s2s shutdown");
            sx_close(conn->s);
        } while(xhash_count(s2s->out));

    if(xhash_iter_first(s2s->in))
        do {
            xhash_iter_get(s2s->in, NULL, xhv.val);
            sx_error(conn->s, stream_err_SYSTEM_SHUTDOWN, "s2s shutdown");
            sx_close(conn->s);
        } while(xhash_count(s2s->in));

    if(xhash_iter_first(s2s->in_accept))
        do {
            xhash_iter_get(s2s->in_accept, NULL, xhv.val);
            sx_close(conn->s);
        } while(xhash_count(s2s->in_accept));


    /* remove dead streams */
    while(jqueue_size(s2s->dead) > 0)
        sx_free((sx_t) jqueue_pull(s2s->dead));

    /* free outgoing queues  */
    xhv.jq_val = &q;
    if(xhash_iter_first(s2s->outq))
        do {
             xhash_iter_get(s2s->outq, NULL, xhv.val);
             jqueue_free(q);
        } while(xhash_iter_next(s2s->outq));

    /* walk & free resolve queues */
    xhv.dns_val = &dns;
    if(xhash_iter_first(s2s->dnscache))
        do {
             xhash_iter_get(s2s->dnscache, NULL, xhv.val);
             free(dns);
        } while(xhash_iter_next(s2s->dnscache));

    /* free hashes */
    xhash_free(s2s->outq);
    xhash_free(s2s->out);
    xhash_free(s2s->in);
    xhash_free(s2s->in_accept);
    xhash_free(s2s->dnscache);

    prep_cache_free(s2s->pc);

    jqueue_free(s2s->dead);

    sx_free(s2s->router);

    sx_env_free(s2s->sx_env);

    mio_free(s2s->mio);

    log_free(s2s->log);

    config_free(s2s->config);

    free(s2s->local_secret);
    free(s2s);

#ifdef POOL_DEBUG
    pool_stat(1);
#endif

    return 0;
}
