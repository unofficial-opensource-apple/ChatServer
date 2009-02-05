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

#include "c2s.h"

#ifdef HAVE_IDN
#include <stringprep.h>
#endif

static sig_atomic_t c2s_shutdown = 0;
sig_atomic_t c2s_lost_router = 0;
static sig_atomic_t c2s_logrotate = 0;

static void _c2s_signal(int signum)
{
    c2s_shutdown = 1;
    c2s_lost_router = 0;
}

static void _c2s_signal_hup(int signum)
{
    c2s_logrotate = 1;
}

/** store the process id */
static void _c2s_pidfile(c2s_t c2s) {
    char *pidfile;
    FILE *f;
    pid_t pid;

    pidfile = config_get_one(c2s->config, "pidfile", 0);
    if(pidfile == NULL)
        return;

    pid = getpid();

    if((f = fopen(pidfile, "w+")) == NULL) {
        log_write(c2s->log, LOG_ERR, "couldn't open %s for writing: %s", pidfile, strerror(errno));
        return;
    }

    if(fprintf(f, "%d", pid) < 0) {
        log_write(c2s->log, LOG_ERR, "couldn't write to %s: %s", pidfile, strerror(errno));
        return;
    }

    fclose(f);

    log_write(c2s->log, LOG_INFO, "process id is %d, written to %s", pid, pidfile);
}
/** pull values out of the config file */
static void _c2s_config_expand(c2s_t c2s)
{
    char *str, *ip, *mask;
    char *max_stanza_scale, *max_message_scale;
    int max_stanza_size, max_message_size;
    config_elem_t elem;
    int i;

    c2s->id = config_get_one(c2s->config, "id", 0);
    if(c2s->id == NULL)
        c2s->id = "c2s";

    c2s->router_ip = config_get_one(c2s->config, "router.ip", 0);
    if(c2s->router_ip == NULL)
        c2s->router_ip = "127.0.0.1";

    c2s->router_port = j_atoi(config_get_one(c2s->config, "router.port", 0), 5347);

    c2s->router_user = config_get_one(c2s->config, "router.user", 0);
    if(c2s->router_user == NULL)
        c2s->router_user = "jabberd";
    c2s->router_pass = config_get_one(c2s->config, "router.pass", 0);
    if(c2s->router_pass == NULL)
        c2s->router_pass = "secret";

    c2s->router_pemfile = config_get_one(c2s->config, "router.pemfile", 0);

    c2s->retry_init = j_atoi(config_get_one(c2s->config, "router.retry.init", 0), 3);
    c2s->retry_lost = j_atoi(config_get_one(c2s->config, "router.retry.lost", 0), 3);
    if((c2s->retry_sleep = j_atoi(config_get_one(c2s->config, "router.retry.sleep", 0), 2)) < 1)
        c2s->retry_sleep = 1;

    c2s->log_type = log_STDOUT;
    if(config_get(c2s->config, "log") != NULL) {
        if((str = config_get_attr(c2s->config, "log", 0, "type")) != NULL) {
            if(strcmp(str, "file") == 0)
                c2s->log_type = log_FILE;
            else if(strcmp(str, "syslog") == 0)
                c2s->log_type = log_SYSLOG;
        }
    }

    if(c2s->log_type == log_SYSLOG) {
        c2s->log_facility = config_get_one(c2s->config, "log.facility", 0);
        c2s->log_ident = config_get_one(c2s->config, "log.ident", 0);
        if(c2s->log_ident == NULL)
            c2s->log_ident = "jabberd/c2s";
    } else if(c2s->log_type == log_FILE)
        c2s->log_ident = config_get_one(c2s->config, "log.file", 0);

    c2s->local_ip = config_get_one(c2s->config, "local.ip", 0);
    if(c2s->local_ip == NULL)
        c2s->local_ip = "0.0.0.0";

    c2s->local_port = j_atoi(config_get_one(c2s->config, "local.port", 0), 0);

    c2s->local_pemfile = config_get_one(c2s->config, "local.pemfile", 0);

    if(config_get(c2s->config, "local.require-starttls") != NULL)
        c2s->local_require_starttls = 1;

    c2s->local_cachain = config_get_one(c2s->config, "local.cachain", 0);

    c2s->local_ssl_port = j_atoi(config_get_one(c2s->config, "local.ssl-port", 0), 0);

    c2s->io_max_fds = j_atoi(config_get_one(c2s->config, "io.max_fds", 0), 1024);

    c2s->max_stanza_bytes = 0;
    elem = config_get(c2s->config, "io.max_stanza_size");
    if (elem != NULL) {
        max_stanza_size = j_atoi(elem->values[0], 0);
        max_stanza_scale = j_attr((const char **) elem->attrs[0], "scale");
        c2s->max_stanza_bytes = sx_scale_limit(max_stanza_size, max_stanza_scale);
    }
	
    c2s->max_message_bytes = 0;
    elem = config_get(c2s->config, "io.max_message_size");
    if (elem != NULL) {
        max_message_size = j_atoi(elem->values[0], 0);
        max_message_scale = j_attr((const char **) elem->attrs[0], "scale");
        c2s->max_message_bytes = sx_scale_limit(max_message_size, max_message_scale);
    }
	
    c2s->io_check_interval = j_atoi(config_get_one(c2s->config, "io.check.interval", 0), 0);
    c2s->io_check_idle = j_atoi(config_get_one(c2s->config, "io.check.idle", 0), 0);
    c2s->io_check_keepalive = j_atoi(config_get_one(c2s->config, "io.check.keepalive", 0), 0);

    c2s->ar_module_name = config_get_one(c2s->config, "authreg.module", 0);

    c2s->ar_register_enable = (config_get(c2s->config, "authreg.register.enable") != NULL);
    if(c2s->ar_register_enable) {
        c2s->ar_register_instructions = config_get_one(c2s->config, "authreg.register.instructions", 0);
        if(c2s->ar_register_instructions == NULL)
            c2s->ar_register_instructions = "Enter a username and password to register with this server.";
    } else
        c2s->ar_register_password = (config_get(c2s->config, "authreg.register.password") != NULL);

    if(config_get(c2s->config, "authreg.mechanisms.traditional.plain") != NULL) c2s->ar_mechanisms |= AR_MECH_TRAD_PLAIN;
    if(config_get(c2s->config, "authreg.mechanisms.traditional.digest") != NULL) c2s->ar_mechanisms |= AR_MECH_TRAD_DIGEST;
    if(config_get(c2s->config, "authreg.mechanisms.traditional.cram-md5") != NULL) c2s->ar_mechanisms |= AR_MECH_TRAD_CRAMMD5;
    if(config_get(c2s->config, "authreg.mechanisms.traditional.zerok") != NULL) c2s->ar_mechanisms |= AR_MECH_TRAD_ZEROK;

    elem = config_get(c2s->config, "io.limits.bytes");
    if(elem != NULL)
    {
        c2s->byte_rate_total = j_atoi(elem->values[0], 0);
        if(c2s->byte_rate_total != 0)
        {
            c2s->byte_rate_seconds = j_atoi(j_attr((const char **) elem->attrs[0], "seconds"), 1);
            c2s->byte_rate_wait = j_atoi(j_attr((const char **) elem->attrs[0], "throttle"), 5);
        }
    }

    elem = config_get(c2s->config, "io.limits.connects");
    if(elem != NULL)
    {
        c2s->conn_rate_total = j_atoi(elem->values[0], 0);
        if(c2s->conn_rate_total != 0)
        {
            c2s->conn_rate_seconds = j_atoi(j_attr((const char **) elem->attrs[0], "seconds"), 5);
            c2s->conn_rate_wait = j_atoi(j_attr((const char **) elem->attrs[0], "throttle"), 5);
        }
    }

    str = config_get_one(c2s->config, "io.access.order", 0);
    if(str == NULL || strcmp(str, "deny,allow") != 0)
        c2s->access = access_new(0);
    else
        c2s->access = access_new(1);

    elem = config_get(c2s->config, "io.access.allow");
    if(elem != NULL)
    {
        for(i = 0; i < elem->nvalues; i++)
        {
            ip = j_attr((const char **) elem->attrs[i], "ip");
            mask = j_attr((const char **) elem->attrs[i], "mask");

            if(ip == NULL)
                continue;

            if(mask == NULL)
                mask = "255.255.255.255";

            access_allow(c2s->access, ip, mask);
        }
    }

    elem = config_get(c2s->config, "io.access.deny");
    if(elem != NULL)
    {
        for(i = 0; i < elem->nvalues; i++)
        {
            ip = j_attr((const char **) elem->attrs[i], "ip");
            mask = j_attr((const char **) elem->attrs[i], "mask");

            if(ip == NULL)
                continue;

            if(mask == NULL)
                mask = "255.255.255.255";

            access_deny(c2s->access, ip, mask);
        }
    }
}

static int _c2s_router_connect(c2s_t c2s) {
    log_write(c2s->log, LOG_NOTICE, "attempting connection to router at %s, port=%d", c2s->router_ip, c2s->router_port);

    c2s->fd = mio_connect(c2s->mio, c2s->router_port, c2s->router_ip, c2s_router_mio_callback, (void *) c2s);
    if(c2s->fd < 0) {
        if(errno == ECONNREFUSED)
            c2s_lost_router = 1;
        log_write(c2s->log, LOG_NOTICE, "connection attempt to router failed: %s (%d)", strerror(errno), errno);
        return 1;
    }

    c2s->router = sx_new(c2s->sx_env, c2s->fd, c2s_router_sx_callback, (void *) c2s);
    sx_client_init(c2s->router, 0, NULL, NULL, NULL, "1.0");

    return 0;
}

static int _c2s_sx_sasl_callback(int cb, void *arg, void **res, sx_t s, void *cbarg) {
    c2s_t c2s = (c2s_t) cbarg;
    char *my_realm, *mech;
    sx_sasl_creds_t creds;
    static char buf[3072];
    char mechbuf[256];
    struct jid_st jid;
    int i, r;

    switch(cb) {
        case sx_sasl_cb_GET_REALM:
            
            if(s->req_to == NULL)   /* this shouldn't happen */
                my_realm = "";

            else {
                my_realm = xhash_get(c2s->realms, s->req_to);
                if(my_realm == NULL)
                    my_realm = s->req_to;
            }

            strncpy(buf, my_realm, 256);
            *res = buf;

            log_debug(ZONE, "sx sasl callback: get realm: realm is '%s'", buf);
            return sx_sasl_ret_OK;
            break;

        case sx_sasl_cb_GET_PASS:
            creds = (sx_sasl_creds_t) arg;

            log_debug(ZONE, "sx sasl callback: get pass (authnid=%s, realm=%s)", creds->authnid, creds->realm);

            if(c2s->ar->get_password && (c2s->ar->get_password)(c2s->ar, (char *)creds->authnid, (creds->realm != NULL) ? (char *)creds->realm: "", buf) == 0) {
                *res = buf;
                return sx_sasl_ret_OK;
	    }

            return sx_sasl_ret_FAIL;

        case sx_sasl_cb_CHECK_PASS:
            creds = (sx_sasl_creds_t) arg;

            log_debug(ZONE, "sx sasl callback: check pass (authnid=%s, realm=%s)", creds->authnid, creds->realm);

            if(c2s->ar->check_password != NULL) {
                if ((c2s->ar->check_password)(c2s->ar, (char *)creds->authnid, (creds->realm != NULL) ? (char *)creds->realm : "", (char *)creds->pass))
                    return sx_sasl_ret_OK;
                else
                    return sx_sasl_ret_FAIL;
            }

            if(c2s->ar->get_password != NULL) {
                if ((c2s->ar->get_password)(c2s->ar, (char *)creds->authnid, (creds->realm != NULL) ? (char *)creds->realm : "", buf) != 0)
                    return sx_sasl_ret_FAIL;

                if (strcmp(creds->pass, buf)==0)
                    return sx_sasl_ret_OK;
            }

            return sx_sasl_ret_FAIL;
            break;
	
        case sx_sasl_cb_CHECK_AUTHZID:

            /* no authzid, we should build one */
            if(creds->authzid == NULL || creds->authzid[0] == '\0') {
                snprintf(buf, 3072, "%s@%s", creds->authnid, s->req_to);
                creds->authzid = (void *)buf;
            }

            /* authzid must be a valid jid */
            jid.pc = c2s->pc;
            if(jid_reset(&jid, creds->authzid, -1) == NULL)
                return sx_sasl_ret_FAIL;

            /* and have domain == stream to addr */
            if(strcmp(jid.domain, s->req_to) != 0)
                return sx_sasl_ret_FAIL;

            /* and have no resource */
            if(jid.resource[0] != '\0')
                return sx_sasl_ret_FAIL;

            /* and exist ! */

            if((c2s->ar->user_exists)(c2s->ar, (char *)creds->authnid, (char *)creds->realm))
                return sx_sasl_ret_OK;

            return sx_sasl_ret_FAIL;

        case sx_sasl_cb_GEN_AUTHZID:

            jid.pc = c2s->pc;
            jid_reset(&jid, s->req_to, -1);

            for(i = 0; i < 256; i++) {
                r = (int) (36.0 * rand() / RAND_MAX);
                jid.node[i] = (r >= 0 && r <= 0) ? (r + 48) : (r + 87);
            }

            jid.node[256] = '\0';

            shahash_r(jid.node, jid.node);

            jid_prep(&jid);

            strcpy(buf, jid_full(&jid));
	
            *res = (void *)buf;

            return sx_sasl_ret_OK;
            break;

        case sx_sasl_cb_CHECK_MECH:
            mech = (char *)arg;

            i=0;
            while(i<sizeof(mechbuf) && mech[i]!='\0') {
                mechbuf[i]=tolower(mech[i]);
                i++;
            }
            mechbuf[i]='\0';

            r = snprintf(buf, sizeof(buf), "authreg.mechanisms.sasl.%s",mechbuf);
            if (r < -1 || r > sizeof(buf))
                return sx_sasl_ret_FAIL;

            /* Work out if our configuration will let us use this mechanism */
            if(config_get(c2s->config,buf) != NULL)
                return sx_sasl_ret_OK;
            else
                return sx_sasl_ret_FAIL;
        default:
            break;
    }

    return sx_sasl_ret_FAIL;
}
static void _c2s_time_checks(c2s_t c2s) {
    sess_t sess;
    time_t now;
    union xhashv xhv;

    now = time(NULL);

    if(xhash_iter_first(c2s->sessions))
        do {
            xhv.sess_val = &sess;
            xhash_iter_get(c2s->sessions, NULL, xhv.val);

            if(c2s->io_check_idle > 0 && now > sess->last_activity + c2s->io_check_idle) {
                log_write(c2s->log, LOG_NOTICE, "[%d] [%s, port=%d] timed out", sess->fd, sess->ip, sess->port);

                sx_error(sess->s, stream_err_HOST_GONE, "connection timed out");
                sx_close(sess->s);

                continue;
            }

            if(c2s->io_check_keepalive > 0 && now > sess->last_activity + c2s->io_check_keepalive && sess->s->state >= state_STREAM) {
                log_debug(ZONE, "sending keepalive for %d", sess->fd);

                sx_raw_write(sess->s, " ", 1);
                
                mio_write(c2s->mio, sess->fd);
            }

        } while(xhash_iter_next(c2s->sessions));
}

int main(int argc, char **argv)
{
    c2s_t c2s;
    char *config_file, *realm;
    char id[1024];
    int i, sd_flags, optchar;
    config_elem_t elem;
    sess_t sess;
    union xhashv xhv;
	int newuid, newgid;
	struct passwd *p;
#ifdef POOL_DEBUG
    time_t pool_time = 0;
#endif
    
#ifdef HAVE_UMASK
    umask((mode_t) 0027);
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

    jabber_signal(SIGINT, _c2s_signal);
    jabber_signal(SIGTERM, _c2s_signal);
#ifdef SIGHUP
    jabber_signal(SIGHUP, _c2s_signal_hup);
#endif
#ifdef SIGPIPE
    jabber_signal(SIGPIPE, SIG_IGN);
#endif

    c2s = (c2s_t) malloc(sizeof(struct c2s_st));
    memset(c2s, 0, sizeof(struct c2s_st));

    /* load our config */
    c2s->config = config_new();

    config_file = CONFIG_DIR "/c2s.xml";

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
                    "c2s - jabberd client-to-server connector (" VERSION ")\n"
                    "Usage: c2s <options>\n"
                    "Options are:\n"
                    "   -c <config>     config file to use [default: " CONFIG_DIR "/c2s.xml]\n"
#ifdef DEBUG
                    "   -D              Show debug output\n"
#endif
                    ,
                    stdout);
                config_free(c2s->config);
                free(c2s);
                return 1;
        }
    }

    if(config_load(c2s->config, config_file) != 0)
    {
        fputs("c2s: couldn't load config, aborting\n", stderr);
        config_free(c2s->config);
        free(c2s);
        return 2;
    }

    _c2s_config_expand(c2s);

    c2s->log = log_new(c2s->log_type, c2s->log_ident, c2s->log_facility);
    log_write(c2s->log, LOG_NOTICE, "starting up");

    _c2s_pidfile(c2s);

    if(c2s->ar_module_name == NULL)
    {
        log_write(c2s->log, LOG_ERR, "no authreg module specified in config file");
        exit(1);
    }

    if((c2s->ar = authreg_init(c2s, c2s->ar_module_name)) == NULL)
        exit(1);

    c2s->pc = prep_cache_new();

    c2s->sessions = xhash_new(1023);

    c2s->conn_rates = xhash_new(101);

    c2s->dead = jqueue_new();

    c2s->sx_env = sx_env_new();

#ifdef HAVE_SSL
    /* get the ssl context up and running */
#ifdef JABBER_USER
    if (seteuid(0)) {
        log_write(c2s->log, LOG_ERR, "cannot seteuid to root: %s", strerror(errno));
        return 1;
    }
#else
    log_write(c2s->log, LOG_NOTICE, "No user is defined for setuid/setgid, continuing");
#endif // JABBER_USER

    if(c2s->local_pemfile != NULL) {
        c2s->sx_ssl = sx_env_plugin(c2s->sx_env, sx_ssl_init, c2s->local_pemfile, c2s->local_cachain);
        if(c2s->sx_ssl == NULL) {
            log_write(c2s->log, LOG_ERR, "failed to load local SSL pemfile, SSL will not be available to clients");
            c2s->local_pemfile = NULL;
        }
    }

    /* try and get something online, so at least we can encrypt to the router */
    if(c2s->sx_ssl == NULL && c2s->router_pemfile != NULL) {
        c2s->sx_ssl = sx_env_plugin(c2s->sx_env, sx_ssl_init, c2s->router_pemfile, NULL);
        if(c2s->sx_ssl == NULL) {
            log_write(c2s->log, LOG_ERR, "failed to load router SSL pemfile, channel to router will not be SSL encrypted");
            c2s->router_pemfile = NULL;
        }
    }
#endif // HAVE_SSL
#ifdef JABBER_USER
    if (setuid(newuid)) {
        log_write(c2s->log, LOG_ERR, "cannot setuid(%d): %s", newuid, strerror(errno));
        return 1;
    }
#else
    log_write(c2s->log, LOG_NOTICE, "No user is defined for setuid/setgid, continuing");
#endif // JABBER_USER

    /* get sasl online */
    sd_flags = 0;

    c2s->sx_sasl = sx_env_plugin(c2s->sx_env, sx_sasl_init, "xmpp", sd_flags, _c2s_sx_sasl_callback, (void *) c2s, sd_flags);
    if(c2s->sx_sasl == NULL) {
        log_write(c2s->log, LOG_ERR, "failed to initialise SASL context, aborting");
        exit(1);
    }

    sx_env_plugin(c2s->sx_env, bind_init);

    c2s->mio = mio_new(c2s->io_max_fds);

    /* realm mapping */
    c2s->realms = xhash_new(51);

    elem = config_get(c2s->config, "local.id");
    for(i = 0; i < elem->nvalues; i++) {
        realm = j_attr((const char **) elem->attrs[i], "realm");

        /* stringprep ids (domain names) so that they are in canonical form */
        strncpy(id, elem->values[i], 1024);
        id[1023] = '\0';
#ifdef HAVE_IDN
        if (stringprep_nameprep(id, 1024) != 0) {
           log_write(c2s->log, LOG_ERR, "cannot stringprep id %s, aborting", id);
           exit(1);
        }
#endif
        xhash_put(c2s->realms, pstrdup(xhash_pool(c2s->realms), id), (realm != NULL) ? realm : pstrdup(xhash_pool(c2s->realms), id));

        log_write(c2s->log, LOG_NOTICE, "[%s] configured; realm=%s", id, realm);
    }

    c2s->sm_avail = xhash_new(51);

    c2s->retry_left = c2s->retry_init;
    _c2s_router_connect(c2s);

    while(!c2s_shutdown) {
        mio_run(c2s->mio, 5);

        if(c2s_logrotate) {
            log_write(c2s->log, LOG_NOTICE, "reopening log ...");
            log_free(c2s->log);
            c2s->log = log_new(c2s->log_type, c2s->log_ident, c2s->log_facility);
            log_write(c2s->log, LOG_NOTICE, "log started");

            c2s_logrotate = 0;
        }

        if(c2s_lost_router) {
            if(c2s->retry_left < 0) {
                log_write(c2s->log, LOG_NOTICE, "attempting reconnect");
                sleep(c2s->retry_sleep);
                c2s_lost_router = 0;
                _c2s_router_connect(c2s);
            }

            else if(c2s->retry_left == 0) {
                c2s_shutdown = 1;
            }

            else {
                log_write(c2s->log, LOG_NOTICE, "attempting reconnect (%d left)", c2s->retry_left);
                c2s->retry_left--;
                sleep(c2s->retry_sleep);
                c2s_lost_router = 0;
                _c2s_router_connect(c2s);
            }
        }
            
        /* cleanup dead sx_ts */
        while(jqueue_size(c2s->dead) > 0)
            sx_free((sx_t) jqueue_pull(c2s->dead));

        /* time checks */
        if(c2s->io_check_interval > 0 && time(NULL) >= c2s->next_check) {
            log_debug(ZONE, "running time checks");

            _c2s_time_checks(c2s);

            c2s->next_check = time(NULL) + c2s->io_check_interval;
            log_debug(ZONE, "next time check at %d", c2s->next_check);
        }

#ifdef POOL_DEBUG
        if(time(NULL) > pool_time + 60) {
            pool_stat(1);
            pool_time = time(NULL);
        }
#endif
    }

    log_write(c2s->log, LOG_NOTICE, "shutting down");
    
    if(xhash_iter_first(c2s->sessions))
        do {
            xhv.sess_val = &sess;
            xhash_iter_get(c2s->sessions, NULL, xhv.val);

            if(sess->active)
                sx_close(sess->s);

        } while(xhash_iter_next(c2s->sessions));

    while(jqueue_size(c2s->dead) > 0)
        sx_free((sx_t) jqueue_pull(c2s->dead));

    sx_free(c2s->router);

    sx_env_free(c2s->sx_env);

    mio_free(c2s->mio);

    xhash_free(c2s->sessions);

    prep_cache_free(c2s->pc);

    authreg_free(c2s->ar);

    xhash_free(c2s->conn_rates);

    xhash_free(c2s->sm_avail);

    xhash_free(c2s->realms);

    jqueue_free(c2s->dead);

    access_free(c2s->access);

    log_free(c2s->log);

    config_free(c2s->config);

    free(c2s);

#ifdef POOL_DEBUG
    pool_stat(1);
#endif

#ifdef HAVE_WINSOCK2_H
    WSACleanup();
#endif

    return 0;
}
