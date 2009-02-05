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

/** @file sm/mod_iq_time.c
  * @brief entity time
  * @author Robert Norris
  * $Date: 2006/03/14 23:27:28 $
  * $Revision: 1.1 $
  */

#ifdef HAVE_TZNAME
extern char *tzname[];
#endif

static mod_ret_t _iq_time_pkt_sm(mod_instance_t mi, pkt_t pkt)
{
    time_t t;
    struct tm *tm;
    char buf[64];
    char *c;

    /* we only want to play with iq:time gets */
    if(pkt->type != pkt_IQ || pkt->ns != ns_TIME)
        return mod_PASS;

    t = time(NULL);

    sm_timestamp(t, buf);
    nad_insert_elem(pkt->nad, 2, NAD_ENS(pkt->nad, 1), "utc", buf);

    tm = localtime(&t);

    strcpy(buf, asctime(tm));
    c = strchr(buf, '\n');
    if(c != NULL)
        *c = '\0';
    nad_insert_elem(pkt->nad, 2, NAD_ENS(pkt->nad, 1), "display", buf);
    
    tzset();
#if defined(HAVE_STRUCT_TM_TM_ZONE)
    nad_insert_elem(pkt->nad, 2, NAD_ENS(pkt->nad, 1), "tz", (char *) tm->tm_zone);
#elif defined(HAVE_TZNAME)
    nad_insert_elem(pkt->nad, 2, NAD_ENS(pkt->nad, 1), "tz", tzname[0]);
#endif

    /* tell them */
    nad_set_attr(pkt->nad, 1, -1, "type", "result", 6);
    pkt_router(pkt_tofrom(pkt));

    return mod_HANDLED;
}

int iq_time_init(mod_instance_t mi, char *arg)
{
    module_t mod = mi->mod;

    if(mod->init) return 0;

    mod->pkt_sm = _iq_time_pkt_sm;

    feature_register(mod->mm->sm, uri_TIME);

    return 0;
}
