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

/* SASL authentication handler */

#include "sx.h"
#include "ssl.h"
#include "sasl.h"
/* Gack - need this otherwise SASL's MD5 definitions conflict with OpenSSLs */
#define MD5_H
#include <sasl/saslplug.h>

/* RFC 3290 defines a number of failure messages */
#define _sasl_err_ABORTED               "aborted"
#define _sasl_err_INCORRECT_ENCODING    "incorrect-encoding"
#define _sasl_err_INVALID_AUTHZID       "invalid-authzid"
#define _sasl_err_INVALID_MECHANISM     "invalid-mechanism"
#define _sasl_err_MECH_TOO_WEAK         "mechanism-too-weak"
#define _sasl_err_NOT_AUTHORIZED        "not-authorized"
#define _sasl_err_TEMPORARY_FAILURE     "temporary-auth-failure"

/* Forward definitions */
static void _sx_sasl_free(sx_t, sx_plugin_t);

/* Support auxprop so that we can use the standard Jabber authreg plugins
 * with SASL mechanisms requiring passwords 
 */
static void _sx_auxprop_lookup(void *glob_context,
			      sasl_server_params_t *sparams,
			      unsigned flags,
			      const char *user,
			      unsigned ulen) {
    char *userid = NULL;
    char *realm  = NULL;
    int ret;
    const struct propval *to_fetch, *current;
    char *user_buf = NULL;
    char *value;
    _sx_sasl_t ctx = (_sx_sasl_t) glob_context;
    struct sx_sasl_creds_st creds = {NULL, NULL, NULL, NULL};

    if (!sparams || !user) 
        return;

    /* It would appear that there's no guarantee that 'user' is NULL 
     * terminated, so we'd better terminate it ... 
     */

    user_buf = sparams->utils->malloc(ulen + 1);
    if (!user_buf)
        goto done;

    memcpy(user_buf, user, ulen);
    user_buf[ulen] = '\0';

    /* Parse the supplied username, splitting it into user and realm
     * components. I suspect that 'parseuser' isn't actually part of the
     * exported API, so maybe we should reimplement this. */

    ret = _plug_parseuser(sparams->utils, &userid, &realm,
                          sparams->user_realm,
                          sparams->serverFQDN, 
                          user_buf);
    if (ret != SASL_OK)
        goto done;
   
 
    /* At present, we only handle fetching the user's password */
    to_fetch = sparams->utils->prop_get(sparams->propctx);
    if (!to_fetch)
        goto done;
    for (current = to_fetch; current->name; current++) {
        if (strcmp(current->name, SASL_AUX_PASSWORD) == 0) {
            /* If we've already got a value, see if we can override it */
            if (current->values) {
                if (flags & SASL_AUXPROP_OVERRIDE) 
                    sparams->utils->prop_erase(sparams->propctx, current->name);
		else
		    continue;
            }

            /* Do the lookup, returning the results into value and value_len */
            if (strcmp(SASL_AUX_PASSWORD_PROP, current->name))  {
                creds.authnid = userid;
                creds.realm = realm;
                if ((ctx->cb)(sx_sasl_cb_GET_PASS, &creds, (void **)&value, 
                              NULL, ctx->cbarg) == sx_sasl_ret_OK) {
                    sparams->utils->prop_set(sparams->propctx, current->name,
                                             value, strlen(value));
                }
            }
        }
    }
 done:
    if (userid) sparams->utils->free(userid);
    if (realm) sparams->utils->free(realm);
    if (user_buf) sparams->utils->free(user_buf);
}

static sasl_auxprop_plug_t _sx_auxprop_plugin = 
    {0, 0, NULL, NULL, _sx_auxprop_lookup, "jabberdsx", NULL};

static int 
sx_auxprop_init(const sasl_utils_t *utils, int max_version, int *out_version,
                sasl_auxprop_plug_t **plug, const char *plugname) {

    if (!out_version || !plug) 
        return SASL_BADPARAM;
    if (max_version < SASL_AUXPROP_PLUG_VERSION ) 
        return SASL_BADVERS;

    *out_version = SASL_AUXPROP_PLUG_VERSION;
    *plug = &_sx_auxprop_plugin;

    return SASL_OK;
}

/* This handles those authreg plugins which won't provide plaintext access
 * to the user's password. Note that there are very few mechanisms which
 * call the verify function, rather than asking for the password
 */
static int _sx_sasl_checkpass(sasl_conn_t *conn, void *ctx, const char *user, const char *pass, unsigned passlen, struct propctx *propctx) {
    _sx_sasl_data_t sd = (_sx_sasl_data_t)ctx;
    struct sx_sasl_creds_st creds = {NULL, NULL, NULL, NULL};

    creds.authnid = user;
    creds.pass = pass;
    
    if (sd->ctx->cb(sx_sasl_cb_CHECK_PASS, &creds, NULL, sd->stream, sd->ctx->cbarg)==sx_sasl_ret_OK) {
        return SASL_OK;
    } else {
        return SASL_BADAUTH;
    }
}

/* Canonicalize the username. Normally this does nothing, but if we're
 * calling from an anonymous plugin, then we need to generate a JID for
 * the user
 */

static int _sx_sasl_canon_user(sasl_conn_t *conn, void *ctx, const char *user, unsigned ulen, unsigned flags, const char *user_realm, char *out_user, unsigned out_umax, unsigned *out_ulen) {
    char *buf;
    _sx_sasl_data_t sd = (_sx_sasl_data_t)ctx;
    sasl_getprop(conn, SASL_MECHNAME, (const void **) &buf);
    if (strcmp(buf, "ANONYMOUS") == 0) {
        sd->ctx->cb(sx_sasl_cb_GEN_AUTHZID, NULL, (void **)&buf, sd->stream, sd->ctx->cbarg);
        strncpy(out_user, buf, out_umax);
        out_user[out_umax]='\0';
        *out_ulen=strlen(out_user);
    } else {
        memcpy(out_user,user,ulen);
        *out_ulen = ulen;
    }
    return SASL_OK;
}

/* Need to make sure that
 *  *) The authnid is permitted to become the given authzid
 *  *) The authnid is included in the given authreg systems DB
 */
static int _sx_sasl_proxy_policy(sasl_conn_t *conn, void *ctx, const char *requested_user, int rlen, const char *auth_identity, int alen, const char *realm, int urlen, struct propctx *propctx) {
    _sx_sasl_data_t sd = (_sx_sasl_data_t) ctx;
    struct sx_sasl_creds_st creds = {NULL, NULL, NULL, NULL};
    char *buf;

    sasl_getprop(conn, SASL_MECHNAME, (const void **) &buf);
    if (strcmp(buf, "ANONYMOUS") == 0) {
        /* If they're anonymous, their ID comes from us, so it must be OK! */
        return SASL_OK;
    } else {
        if (!requested_user || !auth_identity || alen != rlen ||
            (memcmp(requested_user, auth_identity, rlen) !=0)) {
            sasl_seterror(conn, 0,
                          "Requested identity is not authenticated identity");
            return SASL_BADAUTH;
        }
        creds.authnid = auth_identity;
        creds.realm = realm;
        creds.authzid = requested_user;
        /* If we start being fancy and allow auth_identity to be different from
         * requested_user, then this will need to be changed to permit it!
         */
        if ((sd->ctx->cb)(sx_sasl_cb_CHECK_AUTHZID, &creds, NULL, sd->stream, sd->ctx->cbarg)==sx_sasl_ret_OK) 
            return SASL_OK;
        else
            return SASL_BADAUTH;
    }
}

static int _sx_sasl_wio(sx_t s, sx_plugin_t p, sx_buf_t buf) {
    sasl_conn_t *sasl;
    int *x, len, pos, reslen, maxbuf;
    char *out, *result;

    sasl = ((_sx_sasl_data_t) s->plugin_data[p->index])->sasl;

    /* if there's no security layer, don't bother */
    sasl_getprop(sasl, SASL_SSF, (const void **) &x);
    if(*x == 0)
        return 1;

    _sx_debug(ZONE, "doing sasl encode");

    /* can only encode x bytes at a time */
    sasl_getprop(sasl, SASL_MAXOUTBUF, (const void **) &x);
    maxbuf = *x;

    /* encode the output */
    pos = 0;
    result = NULL; reslen = 0;
    while(pos < buf->len) {
        if((buf->len - pos) < maxbuf)
            maxbuf = buf->len - pos;

        sasl_encode(sasl, &buf->data[pos], maxbuf, (const char **) &out, &len);
        
        result = (char *) realloc(result, sizeof(char) * (reslen + len));
        memcpy(&result[reslen], out, len);
        reslen += len;

        pos += maxbuf;
    }
    
    /* replace the buffer */
    _sx_buffer_set(buf, result, reslen, result);

    _sx_debug(ZONE, "%d bytes encoded for sasl channel", buf->len);
    
    return 1;
}

static int _sx_sasl_rio(sx_t s, sx_plugin_t p, sx_buf_t buf) {
    sasl_conn_t *sasl;
    int *x, len;
    char *out;

    sasl = ((_sx_sasl_data_t) s->plugin_data[p->index])->sasl;

    /* if there's no security layer, don't bother */
    sasl_getprop(sasl, SASL_SSF, (const void **) &x);
    if(*x == 0)
        return 1;

    _sx_debug(ZONE, "doing sasl decode");

    /* decode the input */
    sasl_decode(sasl, buf->data, buf->len, (const char **) &out, &len);
    
    /* replace the buffer */
    _sx_buffer_set(buf, out, len, NULL);

    _sx_debug(ZONE, "%d bytes decoded from sasl channel", len);
    
    return 1;
}

/** move the stream to the auth state */
void _sx_sasl_open(sx_t s, sasl_conn_t *sasl) {
    char *method;
    char *buf;
    int *ssf;
    
    /* get the method */
    sasl_getprop(sasl, SASL_MECHNAME, (const void **) &buf);

    method = (char *) malloc(sizeof(char) * (strlen(buf) + 17));
    sprintf(method, "SASL/%s", buf);

    /* get the ssf */
    if(s->ssf == 0) {
        sasl_getprop(sasl, SASL_SSF, (const void **) &ssf);
        s->ssf = *ssf;
    }

    /* and the authenticated id */
    sasl_getprop(sasl, SASL_USERNAME, (const void **) &buf);
        
    /* schwing! */
    sx_auth(s, method, buf);

    free(method);
}

/** make the stream suthenticated second time round */
static void _sx_sasl_stream(sx_t s, sx_plugin_t p) {
    _sx_sasl_t ctx = (_sx_sasl_t) p->private;
    sasl_conn_t *sasl;
    _sx_sasl_data_t sd;
    int ret, i;
    char *realm, *ext_id, *mech;
    sasl_security_properties_t sec_props;

    /* First time around, we need to set up our SASL connection, otherwise
     * features will fall flat on its face */
    if (s->plugin_data[p->index] == NULL) {
        if(s->type == type_SERVER) {

            if(!(s->flags & SX_SASL_OFFER)) {
                _sx_debug(ZONE, "application did not request sasl offer, not offering for this conn");
                return;
            }

            _sx_debug(ZONE, "setting up sasl for this server conn");

            /* Initialise our data object */
            sd = (_sx_sasl_data_t) malloc(sizeof(struct _sx_sasl_data_st));
            memset(sd, 0, sizeof(struct _sx_sasl_data_st));

            /* get the realm */
            if(ctx->cb != NULL)
                (ctx->cb)(sx_sasl_cb_GET_REALM, NULL, (void **) &realm, s, ctx->cbarg);

            /* Initialize our callbacks */
            sd->callbacks = calloc(sizeof(sasl_callback_t),4);

            sd->callbacks[0].id = SASL_CB_PROXY_POLICY;
            sd->callbacks[0].proc = &_sx_sasl_proxy_policy;
            sd->callbacks[0].context = sd;

            sd->callbacks[1].id = SASL_CB_CANON_USER;
            sd->callbacks[1].proc = &_sx_sasl_canon_user;
            sd->callbacks[1].context = sd;

            sd->callbacks[2].id = SASL_CB_SERVER_USERDB_CHECKPASS;
            sd->callbacks[2].proc = &_sx_sasl_checkpass;
            sd->callbacks[2].context = sd;

            sd->callbacks[3].id = SASL_CB_LIST_END;

			/* startup */
// just some comments on these parameters -- note case sensitive
//ctx->appname =            char *principal = "xmpp";
//realm = NULL or hostrealm            char *hostrealm="murata5.apple.com";
//host = NULL or host           char *host = "murata5.apple.com";

            ret = sasl_server_new(ctx->appname, NULL,
                                  realm[0] == '\0' ? NULL : realm,
                                  NULL, NULL, NULL,
                                  ctx->sec_props.security_flags, &sasl);
            if(ret != SASL_OK) {
                _sx_debug(ZONE, "sasl_server_new failed (%s), not offering sasl for this conn", sasl_errstring(ret, NULL, NULL));
                return;
            }

            /* get external data from the ssl plugin */
            ext_id = NULL;
            for(i = 0; i < s->env->nplugins; i++)
                if(s->env->plugins[i]->magic == SX_SASL_SSL_MAGIC)
                    ext_id = (char *) s->plugin_data[s->env->plugins[i]->index];

            /* if we've got some, setup for external auth */
            ret = SASL_OK;
            if(ext_id != NULL) {
                ret = sasl_setprop(sasl, SASL_AUTH_EXTERNAL, ext_id);
                if(ret == SASL_OK) 
                    ret = sasl_setprop(sasl, SASL_SSF_EXTERNAL, &s->ssf);
            }

            /* security properties */
            sec_props = ctx->sec_props;
            if(s->ssf > 0)
                /* if we're already encrypted, then no security layers */
                sec_props.max_ssf = 0;

            if(ret == SASL_OK) 
                ret = sasl_setprop(sasl, SASL_SEC_PROPS, &sec_props);

            if(ret != SASL_OK) {
                _sx_debug(ZONE, "sasl_setprop failed (%s), not offering sasl for this conn", sasl_errstring(ret, NULL, NULL));
                return;
            }

            sd->sasl = sasl;
            sd->stream = s;
            sd->ctx = ctx;

            _sx_debug(ZONE, "sasl context initialised for %d", s->tag);

            s->plugin_data[p->index] = (void *) sd;

        }

        return;
    }

    sasl = ((_sx_sasl_data_t) s->plugin_data[p->index])->sasl;

    /* are we auth'd? */
    if (sasl_getprop(sasl, SASL_MECHNAME, (void *) &mech) == SASL_NOTDONE) {
        _sx_debug(ZONE, "not auth'd, not advancing to auth'd state yet");
        return;
    }

    /* otherwise, its auth time */
    _sx_sasl_open(s, sasl);
}

static void _sx_sasl_features(sx_t s, sx_plugin_t p, nad_t nad) {
    _sx_sasl_data_t sd = (_sx_sasl_data_t) s->plugin_data[p->index];
    int ret, nmechs, ns;
    char *mechs, *mech, *c;

    if(s->type != type_SERVER || sd == NULL || sd->sasl == NULL)
        return;

    if((ret = sasl_getprop(sd->sasl, SASL_MECHNAME, (void *) &mech)) != SASL_NOTDONE) {
        _sx_debug(ZONE, "already auth'd, not offering sasl mechanisms");
        return;
    }

    if(!(s->flags & SX_SASL_OFFER)) {
        _sx_debug(ZONE, "application didn't ask us to offer sasl, so we won't");
        return;
    }

#ifdef HAVE_SSL
    if((s->flags & SX_SSL_STARTTLS_REQUIRE) && s->ssf == 0) {
        _sx_debug(ZONE, "ssl not established yet but the app requires it, not offering mechanisms");
        return;
    }
#endif
    
    _sx_debug(ZONE, "offering sasl mechanisms");

    ret = sasl_listmech(sd->sasl, NULL, "", "|", "", (const char **) &mechs, NULL, &nmechs);
    if(ret != SASL_OK) {
        _sx_debug(ZONE, "sasl_listmech failed (%s), not offering sasl for this conn", sasl_errstring(ret, NULL, NULL));
        _sx_sasl_free(s,p);
        return;
    }
    
    if(nmechs <= 0) {
        _sx_debug(ZONE, "sasl_listmech returned no mechanisms, not offering sasl for this conn");
        _sx_sasl_free(s,p);
        return;
    }

    mech = mechs;
    nmechs = 0;
    while(mech != NULL) {
        c = strchr(mech, '|');
        if(c != NULL)
            *c = '\0';

        if ((sd->ctx->cb)(sx_sasl_cb_CHECK_MECH, mech, NULL, sd->stream, sd->ctx->cbarg)==sx_sasl_ret_OK) {
            if (nmechs == 0) {
                ns = nad_add_namespace(nad, uri_SASL, NULL);
                nad_append_elem(nad, ns, "mechanisms", 1);
            }
            _sx_debug(ZONE, "offering mechanism: %s", mech);

            nad_append_elem(nad, ns, "mechanism", 2);
            nad_append_cdata(nad, mech, strlen(mech), 3);
            nmechs++;
        }

        if(c == NULL)
            mech = NULL;
        else
            mech = ++c;
    }
}

/** utility: generate a success nad */
static nad_t _sx_sasl_success(sx_t s) {
    nad_t nad;
    int ns;

    nad = nad_new(s->nad_cache);
    ns = nad_add_namespace(nad, uri_SASL, NULL);

    nad_append_elem(nad, ns, "success", 0);

    return nad;
}

/** utility: generate a failure nad */
static nad_t _sx_sasl_failure(sx_t s, const char *err) {
    nad_t nad;
    int ns;

    nad = nad_new(s->nad_cache);
    ns = nad_add_namespace(nad, uri_SASL, NULL);

    nad_append_elem(nad, ns, "failure", 0);
    if(err != NULL)
        nad_append_elem(nad, ns, err, 1);

    return nad;
}

/** utility: generate a challenge nad */
static nad_t _sx_sasl_challenge(sx_t s, char *data, int dlen) {
    nad_t nad;
    int ns;

    nad = nad_new(s->nad_cache);
    ns = nad_add_namespace(nad, uri_SASL, NULL);

    nad_append_elem(nad, ns, "challenge", 0);
    if(data != NULL)
        nad_append_cdata(nad, data, dlen, 1);

    return nad;
}

/** utility: generate a response nad */
static nad_t _sx_sasl_response(sx_t s, char *data, int dlen) {
    nad_t nad;
    int ns;

    nad = nad_new(s->nad_cache);
    ns = nad_add_namespace(nad, uri_SASL, NULL);

    nad_append_elem(nad, ns, "response", 0);
    if(data != NULL)
        nad_append_cdata(nad, data, dlen, 1);

    return nad;
}

/** utility: generate an abort nad */
static nad_t _sx_sasl_abort(sx_t s) {
    nad_t nad;
    int ns;

    nad = nad_new(s->nad_cache);
    ns = nad_add_namespace(nad, uri_SASL, NULL);

    nad_append_elem(nad, ns, "abort", 0);

    return nad;
}

/** utility: decode incoming handshake data */
static void _sx_sasl_decode(char *in, int inlen, char **out, int *outlen) {
    *outlen = ap_base64decode_len(in, inlen);
    *out = (char *) malloc(sizeof(char) * (*outlen + 1));
    ap_base64decode(*out, in, inlen);
}

/** utility: encode outgoing handshake data */
static void _sx_sasl_encode(char *in, int inlen, char **out, int *outlen) {
    *outlen = ap_base64encode_len(inlen);
    *out = (char *) malloc(sizeof(char) * *outlen);
    ap_base64encode(*out, in, inlen);
    (*outlen)--;
}

/** auth done, restart the stream */
static void _sx_sasl_notify_success(sx_t s, void *arg) {
    sx_plugin_t p = (sx_plugin_t) arg;

    _sx_chain_io_plugin(s, p);
    _sx_debug(ZONE, "auth completed, resetting");

    _sx_reset(s);

    sx_server_init(s, s->flags);
}

/** process handshake packets from the client */
static void _sx_sasl_client_process(sx_t s, sx_plugin_t p, char *mech, char *in, int inlen) {
    _sx_sasl_data_t sd = (_sx_sasl_data_t) s->plugin_data[p->index];
    char *buf, *out;
    int buflen, outlen, ret;

    if(mech != NULL) {
        _sx_debug(ZONE, "auth request from client (mechanism=%s)", mech);
    } else {
        _sx_debug(ZONE, "response from client");
    }

    /* decode the response */
    _sx_sasl_decode(in, inlen, &buf, &buflen);

    /* process the data */
    if(mech != NULL)
        ret = sasl_server_start(sd->sasl, mech, buf, buflen, (const char **) &out, &outlen);
    else
        ret = sasl_server_step(sd->sasl, buf, buflen, (const char **) &out, &outlen);

    if(buf != NULL) free(buf);

    /* auth completed */
    if(ret == SASL_OK) {
        _sx_debug(ZONE, "sasl handshake completed");

        /* send success */
        _sx_nad_write(s, _sx_sasl_success(s), 0);

        /* set a notify on the success nad buffer */
        ((sx_buf_t) s->wbufq->front->data)->notify = _sx_sasl_notify_success;
        ((sx_buf_t) s->wbufq->front->data)->notify_arg = (void *) p;

        return;
    }

    /* in progress */
    if(ret == SASL_CONTINUE) {
        _sx_debug(ZONE, "sasl handshake in progress (challenge: %.*s)", outlen, out);

        /* encode the challenge */
        _sx_sasl_encode(out, outlen, &buf, &buflen);

        _sx_nad_write(s, _sx_sasl_challenge(s, buf, buflen), 0);

        free(buf);

        return;
    }

    /* its over */
    buf = (char *) sasl_errdetail(sd->sasl);
    if(buf == NULL)
        buf = "[no error message available]";

    _sx_debug(ZONE, "sasl handshake failed: %s", buf);

    _sx_nad_write(s, _sx_sasl_failure(s, _sasl_err_TEMPORARY_FAILURE), 0);
}

/** process handshake packets from the server */
static void _sx_sasl_server_process(sx_t s, sx_plugin_t p, char *in, int inlen) {
    _sx_sasl_data_t sd = (_sx_sasl_data_t)s->plugin_data[p->index];
    char *buf, *out;
    int buflen, outlen, ret;
    const char *err_buf;

    _sx_debug(ZONE, "challenge from client");

    /* decode the response */
    _sx_sasl_decode(in, inlen, &buf, &buflen);

    /* process the data */
    ret = sasl_client_step(sd->sasl, buf, buflen, NULL, (const char **) &out, &outlen);
    if(buf != NULL) free(buf);

    /* in progress */
    if(ret == SASL_OK || ret == SASL_CONTINUE) {
        _sx_debug(ZONE, "sasl handshake in progress (response: %.*s)", outlen, out);

        /* encode the response */
        _sx_sasl_encode(out, outlen, &buf, &buflen);

        _sx_nad_write(s, _sx_sasl_response(s, buf, buflen), 0);

        if(buf != NULL) free(buf);

        return;
    }

    /* its over */
    err_buf = sasl_errdetail(sd->sasl);
    if (err_buf == NULL)
        err_buf = "[no error message available]";
    
    _sx_debug(ZONE, "sasl handshake aborted: %s", err_buf);

    _sx_nad_write(s, _sx_sasl_abort(s), 0);
}

/** main nad processor */
static int _sx_sasl_process(sx_t s, sx_plugin_t p, nad_t nad) {
    _sx_sasl_data_t sd = (_sx_sasl_data_t)s->plugin_data[p->index];
    int attr;
    char mech[128];
    sx_error_t sxe;
    int flags;
    char *ns = NULL, *to = NULL, *from = NULL, *version = NULL;

    /* only want sasl packets */
    if(NAD_ENS(nad, 0) < 0 || NAD_NURI_L(nad, NAD_ENS(nad, 0)) != strlen(uri_SASL) || strncmp(NAD_NURI(nad, NAD_ENS(nad, 0)), uri_SASL, strlen(uri_SASL)) != 0)
        return 1;

    /* quietly drop it if sasl is disabled, or if not ready */
    if(s->state != state_STREAM || sd == NULL) {
        _sx_debug(ZONE, "not correct state for sasl, ignoring");
        nad_free(nad);
        return 0;
    }

    /* packets from the client */
    if(s->type == type_SERVER) {
        if(!(s->flags & SX_SASL_OFFER)) {
            _sx_debug(ZONE, "they tried to do sasl, but we never offered it, ignoring");
            nad_free(nad);
            return 0;
        }

#ifdef HAVE_SSL
        if((s->flags & SX_SSL_STARTTLS_REQUIRE) && s->ssf == 0) {
            _sx_debug(ZONE, "they tried to do sasl, but they have to do starttls first, ignoring");
            nad_free(nad);
            return 0;
        }
#endif

        /* auth */
        if(NAD_ENAME_L(nad, 0) == 4 && strncmp("auth", NAD_ENAME(nad, 0), NAD_ENAME_L(nad, 0)) == 0) {
            /* require mechanism */
            if((attr = nad_find_attr(nad, 0, -1, "mechanism", NULL)) < 0) {
                _sx_nad_write(s, _sx_sasl_failure(s, _sasl_err_INVALID_MECHANISM), 0);
                nad_free(nad);
                return 0;
            }

            /* extract */
            snprintf(mech, 127, "%.*s", NAD_AVAL_L(nad, attr), NAD_AVAL(nad, attr));

            /* go */
            _sx_sasl_client_process(s, p, mech, NAD_CDATA(nad, 0), NAD_CDATA_L(nad, 0));

            nad_free(nad);
            return 0;
        }

        /* response */
        else if(NAD_ENAME_L(nad, 0) == 8 && strncmp("response", NAD_ENAME(nad, 0), NAD_ENAME_L(nad, 0)) == 0) {
            /* process it */
            _sx_sasl_client_process(s, p, NULL, NAD_CDATA(nad, 0), NAD_CDATA_L(nad, 0));

            nad_free(nad);
            return 0;
        }

        /* abort */
        else if(NAD_ENAME_L(nad, 0) == 5 && strncmp("abort", NAD_ENAME(nad, 0), NAD_ENAME_L(nad, 0)) == 0) {
            _sx_debug(ZONE, "sasl handshake aborted");

            _sx_nad_write(s, _sx_sasl_failure(s, _sasl_err_ABORTED), 0);

            nad_free(nad);
            return 0;
        }
    }
    
    /* packets from the server */
    else if(s->type == type_CLIENT) {
        if(sd == NULL) {
            _sx_debug(ZONE, "got sasl client packets, but they never started sasl, ignoring");
            nad_free(nad);
            return 0;
        }

        /* challenge */
        if(NAD_ENAME_L(nad, 0) == 9 && strncmp("challenge", NAD_ENAME(nad, 0), NAD_ENAME_L(nad, 0)) == 0) {
            /* process it */
            _sx_sasl_server_process(s, p, NAD_CDATA(nad, 0), NAD_CDATA_L(nad, 0));

            nad_free(nad);
            return 0;
        }

        /* success */
        else if(NAD_ENAME_L(nad, 0) == 7 && strncmp("success", NAD_ENAME(nad, 0), NAD_ENAME_L(nad, 0)) == 0) {
            _sx_debug(ZONE, "sasl handshake completed, resetting");
            nad_free(nad);

            /* save interesting bits */
            flags = s->flags;

            if(s->ns != NULL) ns = strdup(s->ns);

            if(s->req_to != NULL) to = strdup(s->req_to);
            if(s->req_from != NULL) from = strdup(s->req_from);
            if(s->req_version != NULL) version = strdup(s->req_version);

            /* setup the encoder */
            _sx_chain_io_plugin(s, p);

            /* reset state */
            _sx_reset(s);

            _sx_debug(ZONE, "restarting stream with sasl layer established");

            /* second time round */
            sx_client_init(s, flags, ns, to, from, version);

            /* free bits */
            if(ns != NULL) free(ns);
            if(to != NULL) free(to);
            if(from != NULL) free(from);
            if(version != NULL) free(version);

            return 0;
        }

        /* failure */
        else if(NAD_ENAME_L(nad, 0) == 7 && strncmp("failure", NAD_ENAME(nad, 0), NAD_ENAME_L(nad, 0)) == 0) {
            /* fire the error */
            _sx_gen_error(sxe, SX_ERR_AUTH, "Authentication failed", NULL);
            _sx_event(s, event_ERROR, (void *) &sxe);

            /* cleanup */
            _sx_sasl_free(s,p);

            nad_free(nad);
            return 0;
        }
    }

    /* invalid sasl command, quietly drop it */
    _sx_debug(ZONE, "unknown sasl command '%.*s', ignoring", NAD_ENAME_L(nad, 0), NAD_ENAME(nad, 0));

    nad_free(nad);
    return 0;
}

/** cleanup */
static void _sx_sasl_free(sx_t s, sx_plugin_t p) {
    _sx_sasl_data_t sd = (_sx_sasl_data_t) s->plugin_data[p->index];

    if(sd == NULL)
        return;

    _sx_debug(ZONE, "cleaning up conn state");

    if(sd->sasl != NULL) sasl_dispose(&sd->sasl);
    if(sd->user != NULL) free(sd->user);
    if(sd->psecret != NULL) free(sd->psecret);
    if(sd->callbacks != NULL) free(sd->callbacks);

    free(sd);

    s->plugin_data[p->index] = NULL;
}

static void _sx_sasl_unload(sx_plugin_t p) {

    if (p->private)
        free(p->private);
}

/** args: appname, flags, callback, cb arg */
int sx_sasl_init(sx_env_t env, sx_plugin_t p, va_list args) {
    char *appname;
    int flags;
    sx_sasl_callback_t cb;
    void *cbarg;
    int ret;
    _sx_sasl_t ctx;

    _sx_debug(ZONE, "initialising sasl plugin");

    appname = va_arg(args, char *);
    if(appname == NULL) {
        _sx_debug(ZONE, "appname was NULL, failing");
        return 1;
    }

    flags = va_arg(args, int);

    cb = va_arg(args, sx_sasl_callback_t);
    cbarg = va_arg(args, void *);

    /* Set up the auxiliary property plugin, which we use to gave SASL
     * mechanism plugins access to our passwords
     */
    sasl_auxprop_add_plugin("jabbersx", sx_auxprop_init);

    ctx = (_sx_sasl_t) malloc(sizeof(struct _sx_sasl_st));
    memset(ctx, 0, sizeof(struct _sx_sasl_st));

    ctx->appname = strdup(appname);

    ctx->sec_props.min_ssf = 0;
    ctx->sec_props.max_ssf = -1;
	ctx->sec_props.maxbufsize = 65536;
    ctx->sec_props.security_flags = flags;

    ctx->cb = cb;
    ctx->cbarg = cbarg;
  
    /* Push the location of our callbacks into the auxprop structure */
    
    _sx_auxprop_plugin.glob_context = (void *) ctx;

    ret = sasl_server_init_alt(NULL, appname);
    if(ret != SASL_OK) {
        _sx_debug(ZONE, "sasl_server_init_alt() failed (%s), disabling", sasl_errstring(ret, NULL, NULL));
        return 1;
    }

    _sx_debug(ZONE, "sasl context initialised; appname=%s", appname);

    p->private = (void *) ctx;

    p->unload = _sx_sasl_unload;
    p->wio = _sx_sasl_wio;
    p->rio = _sx_sasl_rio;

    p->stream = _sx_sasl_stream;
    p->features = _sx_sasl_features;
    p->process = _sx_sasl_process;

    p->free = _sx_sasl_free;

    return 0;
}

/* callback functions for client auth */
static int _sx_sasl_cb_get_simple(void *ctx, int id, const char **result, unsigned *len)
{
    _sx_sasl_data_t sd = (_sx_sasl_data_t) ctx;

    _sx_debug(ZONE, "in _sx_sasl_cb_get_simple (id 0x%x)", id);

    *result = sd->user;
    if(len != NULL)
        *len = strlen(*result);

    return SASL_OK;
}

static int _sx_sasl_cb_get_secret(sasl_conn_t *conn, void *ctx, int id, sasl_secret_t **psecret)
{
    _sx_sasl_data_t sd = (_sx_sasl_data_t) ctx;

    _sx_debug(ZONE, "in _sx_sasl_cb_get_secret (id 0x%x)", id);
    
    /* sanity check */
    if(conn == NULL || psecret == NULL || id != SASL_CB_PASS)
        return SASL_BADPARAM;

    *psecret = sd->psecret;

    return SASL_OK;
}

/** kick off the auth handshake */
int sx_sasl_auth(sx_plugin_t p, sx_t s, char *appname, char *mech, char *user, char *pass) {
    _sx_sasl_t ctx = (_sx_sasl_t) p->private;
    _sx_sasl_data_t sd;
    char *buf, *out, *ext_id;
    int i, ret, buflen, outlen, ns;
    sasl_security_properties_t sec_props;
    nad_t nad;

    assert((int) p);
    assert((int) s);
    assert((int) appname);
    assert((int) mech);

    if(s->type != type_CLIENT || s->state != state_STREAM) {
        _sx_debug(ZONE, "need client in stream state for sasl auth");
        return 1;
     }
    
    /* startup */
    ret = sasl_client_init(NULL);
    if(ret != SASL_OK) {
        _sx_debug(ZONE, "sasl_client_init() failed (%s), not authing", sasl_errstring(ret, NULL, NULL));
        return 1;
    }

    sd = (_sx_sasl_data_t) malloc(sizeof(struct _sx_sasl_data_st));
    memset(sd, 0, sizeof(struct _sx_sasl_data_st));

    if(user != NULL)
        sd->user = strdup(user);

    if(pass != NULL) {
        sd->psecret = (sasl_secret_t *) malloc(sizeof(sasl_secret_t) + strlen(pass) + 1);
        strcpy(sd->psecret->data, pass);
        sd->psecret->len = strlen(pass);
    }

    sd->callbacks=calloc(sizeof(sasl_callback_t),4);

    /* authentication name callback */
    sd->callbacks[0].id = SASL_CB_AUTHNAME;
    sd->callbacks[0].proc = &_sx_sasl_cb_get_simple;
    sd->callbacks[0].context = (void *) sd;

    /* password callback */
    sd->callbacks[1].id = SASL_CB_PASS;
    sd->callbacks[1].proc = &_sx_sasl_cb_get_secret;
    sd->callbacks[1].context = (void *) sd;

    /* user identity callback */
    sd->callbacks[2].id = SASL_CB_USER;
    sd->callbacks[2].proc = &_sx_sasl_cb_get_simple;
    sd->callbacks[2].context = (void *) sd;

    /* end of callbacks */
    sd->callbacks[3].id = SASL_CB_LIST_END;
    sd->callbacks[3].proc = NULL;
    sd->callbacks[3].context = NULL;

    /* handshake start */
    ret = sasl_client_new(appname, (s->req_to != NULL) ? s->req_to : "", NULL, NULL, sd->callbacks, 0, &sd->sasl);
    if(ret != SASL_OK) {
        _sx_debug(ZONE, "sasl_client_new failed, (%s), not authing", sasl_errstring(ret, NULL, NULL));

        free(sd->user);
        free(sd->psecret);
        free(sd);

        return 1;
    }

    /* get external data from the ssl plugin */
    ext_id = NULL;
    for(i = 0; i < s->env->nplugins; i++)
        if(s->env->plugins[i]->magic == SX_SASL_SSL_MAGIC)
            ext_id = (char *) s->plugin_data[s->env->plugins[i]->index];

    /* !!! XXX certs */
    /*
    if(ext != NULL) {
        ext->external_id = "foo";
        ext->external_ssf = 20;
    }
    */

    /* if we've got some, setup for external auth */
    ret = SASL_OK;
    if(ext_id != NULL) {
        ret = sasl_setprop(sd->sasl, SASL_AUTH_EXTERNAL, ext_id);
        if(ret == SASL_OK) ret = sasl_setprop(sd->sasl, SASL_SSF_EXTERNAL, &s->ssf);
    }

    /* setup security properties */
    sec_props = ctx->sec_props;
    if(s->ssf > 0)
        /* if we're already encrypted, then no security layers */
        sec_props.max_ssf = 0;

    ret = sasl_setprop(sd->sasl, SASL_SEC_PROPS, &sec_props);
    if(ret != SASL_OK) {
        _sx_debug(ZONE, "sasl_setprop failed (%s), not authing", sasl_errstring(ret, NULL, NULL));

        sasl_dispose(&sd->sasl);

        free(sd->user);
        free(sd->psecret);
        free(sd);

        return 1;
    }

    /* handshake start */
    ret = sasl_client_start(sd->sasl, mech, NULL, (const char **) &out, &outlen, NULL);
    if(ret != SASL_OK && ret != SASL_CONTINUE) {
        _sx_debug(ZONE, "sasl_client_start failed (%s), not authing", sasl_errstring(ret, NULL, NULL));

        sasl_dispose(&sd->sasl);

        free(sd->user);
        free(sd->psecret);
        free(sd);

        return 1;
    }

    /* save userdata */
    s->plugin_data[p->index] = (void *) sd;

    /* in progress */
    _sx_debug(ZONE, "sending auth request to server, mech '%s': %.*s", mech, outlen, out);

    /* encode the challenge */
    _sx_sasl_encode(out, outlen, &buf, &buflen);

    /* build the nad */
    nad = nad_new(s->nad_cache);
    ns = nad_add_namespace(nad, uri_SASL, NULL);

    nad_append_elem(nad, ns, "auth", 0);
    nad_append_attr(nad, -1, "mechanism", mech);
    if(buf != NULL) {
        nad_append_cdata(nad, buf, buflen, 1);
        free(buf);
    }

    /* its away */
    sx_nad_write(s, nad);

    return 0;
}
