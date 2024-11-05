// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Zebra dataplane plugin for Grout
 *
 * Copyright (C) 2024 Red Hat
 * Christophe Fontaine
 */
#include <zebra.h>

#include "lib/json.h"

#include "zebra/grout/zebra_dplane_grout.h"
#include "zebra/grout/zebra_dplane_grout_vty_clippy.c"

#define ZD_STR "Zebra dataplane information\n"
#define ZD_GROUT_STR "Grout information\n"

DEFPY (zd_grout_show_ports,
       zd_grout_show_ports_cmd,
       "show dplane grout port [(1-32)$port_id] [detail$detail] [json$json]",
       SHOW_STR
       ZD_STR
       ZD_GROUT_STR
       "show port info\n"
       "Grout port identifier\n"
       "Detailed information\n"
       JSON_STR)
{
	bool uj = !!json;
	bool ud = !!detail;

	zd_grout_port_show(vty, port_id, uj, ud);

	return CMD_SUCCESS;
}

void zd_grout_vty_init(void)
{
	install_element(VIEW_NODE, &zd_grout_show_ports_cmd);
}
