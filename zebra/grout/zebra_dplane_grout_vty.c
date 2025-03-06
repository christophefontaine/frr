// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Zebra dataplane plugin for Grout
 *
 * Copyright (C) 2024 Red Hat
 * Christophe Fontaine
 */
#include <zebra.h>
#include <zebra/debug.h>

#include "lib/json.h"

#include "zebra/grout/zebra_dplane_grout.h"
#include "zebra/grout/zebra_dplane_grout_vty_clippy.c"

#include <gr_infra.h>

#define ZD_STR "Zebra dataplane information\n"
#define ZD_GROUT_STR "Grout information\n"

extern unsigned long int zebra_debug_dplane_grout;
extern struct grout_ctx_t grout_ctx;

DEFPY(zebra_grout_port_add, zebra_grout_port_add_del_cmd,
      "[no$no] grout interface pci WORD$pci_addr",
	NO_STR
	"Grout\n"
	"Interface management\n"
	"physical interface (pci address)"
	"pci address\n"
	"\n")
{
	bool delete = no;
	printf("add pci interface %s\n", pci_addr);
	return CMD_SUCCESS;
}

DEFPY(zebra_grout_vlan_add, zebra_grout_vlan_add_del_cmd,
      "[no$no] grout interface vlan (1-4095)$id parent IFNAME",
	NO_STR
	"Grout\n"
	"Interface management\n"
	"vlan id\n"
	"parent interface\n"
	INTERFACE_STR
	"\n")
{
	bool delete = no;
	printf("add vlan %ld on iface %s\n", id, ifname);
	return CMD_SUCCESS;
}


DEFPY(debug_zebra_dplane_grout, debug_zebra_dplane_grout_cmd,
      "[no$no] debug zebra dplane grout [detailed$detail]",
      NO_STR DEBUG_STR
      "Zebra configuration\n"
      "Debug zebra dataplane events\n"
      "Detailed debug information\n"
      "\n")
{
	if (no) {
		UNSET_FLAG(zebra_debug_dplane_grout, ZEBRA_DEBUG_DPLANE_GROUT);
		UNSET_FLAG(zebra_debug_dplane_grout,
			   ZEBRA_DEBUG_DPLANE_GROUT_DETAIL);
	} else {
		SET_FLAG(zebra_debug_dplane_grout, ZEBRA_DEBUG_DPLANE_GROUT);

		if (detail)
			SET_FLAG(zebra_debug_dplane_grout,
				 ZEBRA_DEBUG_DPLANE_GROUT_DETAIL);
	}

	return CMD_SUCCESS;
}


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
	install_element(CONFIG_NODE, &zebra_grout_port_add_del_cmd);
	install_element(CONFIG_NODE, &zebra_grout_vlan_add_del_cmd);
}
