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

#include "sm.h"

/** @file sm/mod_presence.c
  * @brief presence tracker driver
  * @author Robert Norris
  * $Date: 2006/03/14 23:27:28 $
  * $Revision: 1.1 $
  */

/** presence from the session */
static mod_ret_t _presence_in_sess(mod_instance_t mi, sess_t sess, pkt_t pkt) {
    /* only handle presence */
    if(!(pkt->type & pkt_PRESENCE))
        return mod_PASS;

    /* reset from if necessary */
    if(pkt->from == NULL || jid_compare_user(pkt->from, sess->jid) != 0) {
        if(pkt->from != NULL)
            jid_free(pkt->from);

        pkt->from = jid_dup(sess->jid);
        nad_set_attr(pkt->nad, 1, -1, "from", jid_full(pkt->from), 0);
    }

    /* presence broadcast (T1, T2, T3) */
    if(pkt->to == NULL)
        pres_update(sess, pkt);

    /* directed presence (T7, T8) */
    else
        pres_deliver(sess, pkt);

    return mod_HANDLED;
}

/* drop incoming presence if the user isn't around,
 * so we don't have to load them during broadcasts */
mod_ret_t _presence_in_router(mod_instance_t mi, pkt_t pkt) {
    user_t user;
    sess_t sess;

    /* only check presence */
    if(!(pkt->type & pkt_PRESENCE))
        return mod_PASS;

    /* get the user _without_ doing a load */
    user = xhash_get(mi->mod->mm->sm->users, jid_user(pkt->to));

    /* no user, or no sessions, bail */
    if(user == NULL || user->sessions == NULL) {
        pkt_free(pkt);
        return mod_HANDLED;
    }

    /* only pass if there's at least one available session */
    for(sess = user->sessions; sess != NULL; sess = sess->next)
        if(sess->available && sess->pri >= 0)
            return mod_PASS;

    /* no available sessions, drop */
    pkt_free(pkt);

    return mod_HANDLED;
}

/** presence to a user */
static mod_ret_t _presence_pkt_user(mod_instance_t mi, user_t user, pkt_t pkt) {
    sess_t sess;

    /* only handle presence */
    if(!(pkt->type & pkt_PRESENCE))
        return mod_PASS;

    /* errors get tracked, but still delivered (T6) */
    if(pkt->type & pkt_ERROR) {
        /* find the session */
        sess = sess_match(user, pkt->to->resource);
        if(sess == NULL) {
            log_debug(ZONE, "bounced presence, but no corresponding session anymore, dropping");
            pkt_free(pkt);
            return mod_HANDLED;
        }
            
        log_debug(ZONE, "bounced presence, tracking");
        pres_error(sess, pkt->from);

        /* bounced probes get dropped */
        if((pkt->type & pkt_PRESENCE_PROBE) == pkt_PRESENCE_PROBE) {
            pkt_free(pkt);
            return mod_HANDLED;
        }
    }

    /* someone sent us a raw invisible? hrm */
    if(pkt->type == pkt_PRESENCE_INVIS) {
        log_debug(ZONE, "urgh, broken server sent us an invisible, rewriting it");
        nad_set_attr(pkt->nad, 1, -1, "type", "unavailable", 11);
        pkt->type = pkt_PRESENCE_UN;
    }

    /* if there's a resource, send it direct */
    if(*pkt->to->resource != '\0') {
        sess = sess_match(user, pkt->to->resource);
        if(sess == NULL)
            /* this resource isn't online */
            return -stanza_err_RECIPIENT_UNAVAILABLE;   /* xmpp-im-11 9.5#2 */

        pkt_sess(pkt, sess);
        return mod_HANDLED;
    }

    /* remote presence updates (T4, T5) */
    pres_in(user, pkt);

    return mod_HANDLED;
}

int presence_init(mod_instance_t mi, char *arg) {
    module_t mod = mi->mod;

    if(mod->init) return 0;

    mod->in_sess = _presence_in_sess;
    mod->in_router = _presence_in_router;
    mod->pkt_user = _presence_pkt_user;

    feature_register(mod->mm->sm, "presence");
    feature_register(mod->mm->sm, "presence-invisible");

    return 0;
}
