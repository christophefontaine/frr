// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Zebra dataplane plugin for Grout
 *
 * Copyright (C) 2024 Red Hat
 * Christophe Fontaine
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* Include this explicitly */
#endif
#include "zebra_dplane_grout.h"

#include "lib/libfrr.h"

#include "zebra/debug.h"
#include "zebra/interface.h"
#include "zebra/zebra_dplane.h"
#include "zebra/debug.h"
#include "zebra/zebra_pbr.h"

#include <gr_infra.h>
#include <gr_api_client_impl.h>
#include <gr_ip4.h>
#include <gr_ip6.h>

struct grout_ctx_t {
	struct gr_api_client *client;
	int fd;
	int ports;
};

static struct grout_ctx_t grout_ctx;
static const char *plugin_name = "zebra_dplane_grout";

DEFINE_MTYPE_STATIC(ZEBRA, GROUT_PORTS, "ZD Grout port database");


/* Grout provider callback.
 */
static void zd_grout_process_update(struct zebra_dplane_ctx *ctx)
{
	switch (dplane_ctx_get_op(ctx)) {

	case DPLANE_OP_RULE_ADD:
	case DPLANE_OP_RULE_UPDATE:
	case DPLANE_OP_RULE_DELETE:
		break;
	case DPLANE_OP_NONE:
	case DPLANE_OP_ROUTE_INSTALL:
	case DPLANE_OP_ROUTE_UPDATE:
	case DPLANE_OP_ROUTE_DELETE:
	case DPLANE_OP_ROUTE_NOTIFY:
	case DPLANE_OP_NH_INSTALL:
	case DPLANE_OP_NH_UPDATE:
	case DPLANE_OP_NH_DELETE:
	case DPLANE_OP_LSP_INSTALL:
	case DPLANE_OP_LSP_UPDATE:
	case DPLANE_OP_LSP_DELETE:
	case DPLANE_OP_LSP_NOTIFY:
	case DPLANE_OP_PW_INSTALL:
	case DPLANE_OP_PW_UNINSTALL:
		break;
	case DPLANE_OP_SYS_ROUTE_ADD:
		printf("DPLANE_OP_SYS_ROUTE_ADD");
		break;
	case DPLANE_OP_SYS_ROUTE_DELETE:
	case DPLANE_OP_ADDR_INSTALL:
		printf("DPLANE_OP_ADDR_INSTALL");
		break;
	case DPLANE_OP_ADDR_UNINSTALL:
		printf("DPLANE_OP_ADDR_UNINSTALL");
		break;
	case DPLANE_OP_MAC_INSTALL:
	case DPLANE_OP_MAC_DELETE:
	case DPLANE_OP_NEIGH_INSTALL:
	case DPLANE_OP_NEIGH_UPDATE:
	case DPLANE_OP_NEIGH_DELETE:
	case DPLANE_OP_VTEP_ADD:
	case DPLANE_OP_VTEP_DELETE:
	case DPLANE_OP_NEIGH_DISCOVER:
	case DPLANE_OP_BR_PORT_UPDATE:
	case DPLANE_OP_IPTABLE_ADD:
	case DPLANE_OP_IPTABLE_DELETE:
	case DPLANE_OP_IPSET_ADD:
	case DPLANE_OP_IPSET_DELETE:
	case DPLANE_OP_IPSET_ENTRY_ADD:
	case DPLANE_OP_IPSET_ENTRY_DELETE:
	case DPLANE_OP_NEIGH_IP_INSTALL:
	case DPLANE_OP_NEIGH_IP_DELETE:
	case DPLANE_OP_NEIGH_TABLE_UPDATE:
	case DPLANE_OP_GRE_SET:
		break;
	case DPLANE_OP_INTF_ADDR_ADD:
		printf("DPLANE_OP_INTF_ADDR_ADD");
		break;
	case DPLANE_OP_INTF_ADDR_DEL:
		printf("DPLANE_OP_INTF_ADDR_DEL");
		break;
	case DPLANE_OP_INTF_NETCONFIG:
	case DPLANE_OP_INTF_INSTALL:
	case DPLANE_OP_INTF_UPDATE:
	case DPLANE_OP_INTF_DELETE:
	case DPLANE_OP_VLAN_INSTALL:
		break;
	}
	printf("grout: %s\n", dplane_op2str(dplane_ctx_get_op(ctx)));
}


static int zd_grout_process(struct zebra_dplane_provider *prov)
{
	struct zebra_dplane_ctx *ctx;
	int counter, limit;

	if (IS_ZEBRA_DEBUG_DPLANE_GROUT_DETAIL)
		zlog_debug("processing %s", dplane_provider_get_name(prov));

	limit = dplane_provider_get_work_limit(prov);
	for (counter = 0; counter < limit; counter++) {
		ctx = dplane_provider_dequeue_in_ctx(prov);
		if (!ctx)
			break;

		zd_grout_process_update(ctx);
		dplane_ctx_set_status(ctx, ZEBRA_DPLANE_REQUEST_SUCCESS);
		dplane_provider_enqueue_out_ctx(prov, ctx);
	}

	return 0;
}

void zd_grout_port_show(struct vty *vty, uint16_t port_id, bool uj, int detail)
{
	/* XXX - support for json is yet to be added */
	if (uj)
		return;

	if (!detail) {
		vty_out(vty, "%-4s %-16s %-16s %-16s %s\n", "Port", "Device",
			"IfName", "IfIndex", "sw,domain,port");
	}
}


static void zd_grout_port_init(void)
{
	struct gr_infra_iface_list_req req = {.type = GR_IFACE_TYPE_UNDEF};
	struct gr_infra_iface_list_resp *resp;
	void * resp_ptr = NULL;

	if (IS_ZEBRA_DEBUG_DPLANE_GROUT)
		zlog_debug("grout port init");

	if (gr_api_client_send_recv(grout_ctx.client, GR_INFRA_IFACE_LIST, sizeof(req), &req, &resp_ptr) < 0) {
		return;
	}
	resp = resp_ptr;

	if (resp->n_ifaces == 0) {
		if (IS_ZEBRA_DEBUG_DPLANE_GROUT)
			zlog_debug("no probed ethernet devices");
	}

	for (int i = 0; i < resp->n_ifaces; i++) {
		struct interface *iface = if_get_by_name(resp->ifaces[i].name, resp->ifaces[i].vrf_id, NULL);
		iface->status = ZEBRA_INTERFACE_ACTIVE | ZEBRA_INTERFACE_LINKDETECTION;
		iface->flags = IFF_RUNNING | IFF_MULTICAST | IFF_BROADCAST | IFF_UP; // IFF_PROMISC | IFF_ALLMULTI | IFF_IPV4 | 
		iface->speed = 25000;
		iface->bandwidth = iface->speed * 1000;
		iface->metric = 100;
		iface->mtu = resp->ifaces[i].mtu;
		if (resp->ifaces[i].type == GR_IFACE_TYPE_PORT) {
			memcpy(iface->hw_addr, &((struct gr_iface_info_port *)resp->ifaces[i].info)->mac, 6);
			iface->hw_addr_len = 6;
			iface->ll_type = ZEBRA_LLT_ETHER;
		} else if (resp->ifaces[i].type == GR_IFACE_TYPE_VLAN) {
			memcpy(iface->hw_addr, &((struct gr_iface_info_vlan *)resp->ifaces[i].info)->mac, 6);
			iface->hw_addr_len = 6;
			iface->ll_type = ZEBRA_LLT_ETHER;
		}
	}
}

static int zd_grout_start(struct zebra_dplane_provider *prov)
{
	if (IS_ZEBRA_DEBUG_DPLANE_GROUT)
		zlog_debug("%s start", dplane_provider_get_name(prov));

	zd_grout_vty_init();

	frr_with_privs (&zserv_privs) {
		zd_grout_port_init();
	}

	return 0;
}

static int zd_grout_finish(struct zebra_dplane_provider *prov, bool early)
{
	gr_api_client_disconnect(grout_ctx.client);
	return 0;
}


static int zd_grout_plugin_init(struct event_loop *tm)
{
	int ret;

	ret = dplane_provider_register(
		plugin_name, DPLANE_PRIO_KERNEL, DPLANE_PROV_FLAGS_DEFAULT,
		zd_grout_start, zd_grout_process, zd_grout_finish, &grout_ctx, NULL);

	if (IS_ZEBRA_DEBUG_DPLANE_GROUT)
		zlog_debug("%s register status %d", plugin_name, ret);

	grout_ctx.client = gr_api_client_connect(GR_DEFAULT_SOCK_PATH);

	return 0;
}


static int zd_grout_module_init(void)
{
	hook_register(frr_late_init, zd_grout_plugin_init);
	return 0;
}

FRR_MODULE_SETUP(
	.name = "dplane_grout",
	.version = "0.0.1",
	.description = "Data plane plugin using grout",
	.init = zd_grout_module_init,
);
