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

/** @file sm/mod_iq_private.c
  * @brief private xml storage
  * @author Robert Norris
  * $Date: 2006/03/14 23:27:28 $
  * $Revision: 1.1 $
  */

static mod_ret_t _iq_private_in_sess(mod_instance_t mi, sess_t sess, pkt_t pkt) {
    int ns, elem, target, targetns;
    st_ret_t ret;
    char filter[4096];
    os_t os;
    os_object_t o;
    nad_t nad;
    pkt_t result;

    /* only handle private sets and gets */
    if((pkt->type != pkt_IQ && pkt->type != pkt_IQ_SET) || pkt->ns != ns_PRIVATE)
        return mod_PASS;

    /* we're only interested in no to, to our host, or to us */
    if(pkt->to != NULL && jid_compare_user(sess->jid, pkt->to) != 0 && strcmp(sess->jid->domain, jid_user(pkt->to)) != 0)
        return mod_PASS;

    ns = nad_find_scoped_namespace(pkt->nad, uri_PRIVATE, NULL);
    elem = nad_find_elem(pkt->nad, 1, ns, "query", 1);

    /* find the first child */
    target = elem + 1;
    while(target < pkt->nad->ecur)
    {
        if(pkt->nad->elems[target].depth > pkt->nad->elems[elem].depth)
            break;

        target++;
    }

    /* not found, so we're done */
    if(target == pkt->nad->ecur)
        return -stanza_err_BAD_REQUEST;

    /* find the target namespace */
    targetns = NAD_ENS(pkt->nad, target);

    /* gotta have a namespace */
    if(targetns < 0)
    {
        log_debug(ZONE, "no namespace specified");
        return -stanza_err_BAD_REQUEST;
    }

    log_debug(ZONE, "processing private request for %.*s", NAD_NURI_L(pkt->nad, targetns), NAD_NURI(pkt->nad, targetns));

    /* get */
    if(pkt->type == pkt_IQ) {
        snprintf(filter, 4096, "(ns=%i:%.*s)", NAD_NURI_L(pkt->nad, targetns), NAD_NURI_L(pkt->nad, targetns), NAD_NURI(pkt->nad, targetns));
        ret = storage_get(sess->user->sm->st, "private", jid_user(sess->jid), filter, &os);
        switch(ret) {
            case st_SUCCESS:
                if(os_iter_first(os)) {
                    o = os_iter_object(os);
                    if(os_object_get_nad(os, o, "xml", &nad)) {
                        result = pkt_new(sess->user->sm, nad_copy(nad));
                        if(result != NULL) {
                            nad_set_attr(result->nad, 1, -1, "type", "result", 6);

                            pkt_id(pkt, result);

                            pkt_sess(result, sess);

                            pkt_free(pkt);

                            os_free(os);
                
                            return mod_HANDLED;
                        }
                    }
                }

                os_free(os);

                /* drop through */
                log_debug(ZONE, "storage_get succeeded, but couldn't make packet, faking st_NOTFOUND");

            case st_NOTFOUND:

                log_debug(ZONE, "namespace not found, returning");

                /*
                 * !!! really, we should just return a 404. 1.4 just slaps a
                 *     result on the packet and sends it back. hurrah for
                 *     legacy namespaces.
                 */
                nad_set_attr(pkt->nad, 1, -1, "type", "result", 6);

                pkt_sess(pkt_tofrom(pkt), sess);
                
                return mod_HANDLED;

            case st_FAILED:
                return -stanza_err_INTERNAL_SERVER_ERROR;

            case st_NOTIMPL:
                return -stanza_err_FEATURE_NOT_IMPLEMENTED;
        }
    }

    os = os_new();
    o = os_object_new(os);

    snprintf(filter, 4096, "%.*s", NAD_NURI_L(pkt->nad, targetns), NAD_NURI(pkt->nad, targetns));
    os_object_put(o, "ns", filter, os_type_STRING);
    os_object_put(o, "xml", pkt->nad, os_type_NAD);

    snprintf(filter, 4096, "(ns=%i:%.*s)", NAD_NURI_L(pkt->nad, targetns), NAD_NURI_L(pkt->nad, targetns), NAD_NURI(pkt->nad, targetns));

    ret = storage_replace(sess->user->sm->st, "private", jid_user(sess->jid), filter, os);
    os_free(os);

    switch(ret) {
        case st_FAILED:
            return -stanza_err_INTERNAL_SERVER_ERROR;

        case st_NOTIMPL:
            return -stanza_err_FEATURE_NOT_IMPLEMENTED;

        default:
            result = pkt_create(sess->user->sm, "iq", "result", NULL, NULL);

            pkt_id(pkt, result);

            pkt_sess(result, sess);
            
            pkt_free(pkt);

            return mod_HANDLED;
    }

    /* we never get here */
    return 0;
}

static void _iq_private_user_delete(mod_instance_t mi, jid_t jid) {
    log_debug(ZONE, "deleting private xml storage for %s", jid_user(jid));

    storage_delete(mi->sm->st, "private", jid_user(jid), NULL);
}

int iq_private_init(mod_instance_t mi, char *arg) {
    module_t mod = mi->mod;

    if(mod->init) return 0;

    mod->in_sess = _iq_private_in_sess;
    mod->user_delete = _iq_private_user_delete;

    feature_register(mod->mm->sm, uri_PRIVATE);

    return 0;
}
