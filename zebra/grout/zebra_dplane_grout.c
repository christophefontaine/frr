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
#include <net/if.h>
#ifdef GNU_LINUX
#include <linux/if.h>
#endif /* GNU_LINUX */

#include "zebra_dplane_grout.h"

#include "lib/libfrr.h"
#include "lib/frr_pthread.h"

#include "zebra/connected.h"
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
	struct gr_api_client *notifs;
	struct frr_pthread *dg_pthread;
	/* Event/'thread' pointer for queued updates */
	struct event *dg_t_update;
	_Atomic bool dg_run;
};

static struct grout_ctx_t grout_ctx;
static const char *plugin_name = "zebra_dplane_grout";

DEFINE_MTYPE_STATIC(ZEBRA, GROUT_PORTS, "ZD Grout port database");

static void dplane_read_notifications(struct event *event) {
	zlog_debug("grout dplane_read_notifications");

//       event_add_read(grout_ctx.dg_pthread->master, dplane_read_notifications,
//		NULL, 0, &grout_ctx.dg_t_update);
}

static enum zebra_dplane_result zd_grout_add_del_address(struct zebra_dplane_ctx *ctx) {
	int vrf = dplane_ctx_get_vrf(ctx);
	int iface_index = dplane_ctx_get_ifindex(ctx);
	struct interface *iface = if_lookup_by_index(iface_index, vrf);
	int iface_id = iface_index - 1000;
	const struct prefix *p = dplane_ctx_get_intf_addr(ctx);

	if (p->family == AF_INET) {
		if (dplane_ctx_get_op(ctx) == DPLANE_OP_ADDR_INSTALL ||
   		    dplane_ctx_get_op(ctx) == DPLANE_OP_INTF_ADDR_ADD) {
	        struct gr_ip4_addr_add_req req = {.exist_ok = true};
		req.addr.addr.ip = p->u.prefix4.s_addr;
		req.addr.addr.prefixlen = p->prefixlen;
		req.addr.iface_id = iface_id;
	        if (gr_api_client_send_recv(grout_ctx.client, GR_IP4_ADDR_ADD, sizeof(req), &req, NULL) < 0)
			zlog_debug("Grout error add IP.");
		else {
			connected_add_ipv4(iface, 0, &p->u.prefix4, p->prefixlen, NULL,
						NULL, 100);
			return ZEBRA_DPLANE_REQUEST_SUCCESS;
		}
		} else {
	        	struct gr_ip4_addr_del_req req = {.missing_ok = true};
			req.addr.addr.ip = p->u.prefix4.s_addr;
			req.addr.addr.prefixlen = p->prefixlen;
			req.addr.iface_id = iface_id;
	        	if (gr_api_client_send_recv(grout_ctx.client, GR_IP4_ADDR_DEL, sizeof(req), &req, NULL) < 0)
				zlog_debug("Grout error deleting IP.");
			else {
				connected_delete_ipv4(iface, 0, &p->u.prefix4, p->prefixlen, NULL);
				return ZEBRA_DPLANE_REQUEST_SUCCESS;
			}
		}
	}
	else if (p->family == AF_INET6) {
		if (dplane_ctx_get_op(ctx) == DPLANE_OP_ADDR_INSTALL ||
   		    dplane_ctx_get_op(ctx) == DPLANE_OP_INTF_ADDR_ADD) {
	       		struct gr_ip6_addr_add_req req = {.exist_ok = true};
			memcpy(req.addr.addr.ip.a, p->u.prefix6.s6_addr, sizeof(req.addr.addr.ip.a));
			req.addr.addr.prefixlen = p->prefixlen;
			req.addr.iface_id = iface_id;
	        	if (gr_api_client_send_recv(grout_ctx.client, GR_IP6_ADDR_ADD, sizeof(req), &req, NULL) < 0)
				zlog_debug("Grout error add ipv6");
			else {
				connected_add_ipv6(iface, 0, &p->u.prefix6, NULL,
						   p->prefixlen, NULL, 100);
				return ZEBRA_DPLANE_REQUEST_SUCCESS;
			}
		} else {
	       		struct gr_ip6_addr_del_req req = {.missing_ok = true};
			memcpy(req.addr.addr.ip.a, p->u.prefix6.s6_addr, sizeof(req.addr.addr.ip.a));
			req.addr.addr.prefixlen = p->prefixlen;
			req.addr.iface_id = iface_id;
	        	if (gr_api_client_send_recv(grout_ctx.client, GR_IP6_ADDR_DEL, sizeof(req), &req, NULL) < 0)
				zlog_debug("Grout error del ipv6");
			else {
				connected_delete_ipv6(iface, &p->u.prefix6, NULL, p->prefixlen);
				return ZEBRA_DPLANE_REQUEST_SUCCESS;
			}
		}
	} else {
		zlog_debug("Error, family is neither AF_INET or AF_INET6");
	}
	return ZEBRA_DPLANE_REQUEST_FAILURE;
}

static enum zebra_dplane_result zd_grout_add_del_nexthop(struct zebra_dplane_ctx *ctx) {
	// dummy function, NHs are created automatically bu grout ?
	// FIXME: shouldn't return unconditionnaly, and should filter by interface
	return ZEBRA_DPLANE_REQUEST_SUCCESS;
}

static enum zebra_dplane_result zd_grout_add_del_route(struct zebra_dplane_ctx *ctx) {
	const struct nexthop_group *ng = dplane_ctx_get_ng(ctx);
	const struct prefix *p, *src_p;
	int vrf_id;

        p = dplane_ctx_get_dest(ctx);
        src_p = dplane_ctx_get_src(ctx);
	vrf_id = dplane_ctx_get_vrf(ctx);

	if (dplane_ctx_get_op(ctx) == DPLANE_OP_SYS_ROUTE_ADD ||
	    dplane_ctx_get_op(ctx) == DPLANE_OP_ROUTE_INSTALL ||
	    dplane_ctx_get_op(ctx) == DPLANE_OP_ROUTE_UPDATE) {
		if (p->family == AF_INET) {
			struct gr_ip4_route_add_req req = {.exist_ok = true};
			req.dest.ip = p->u.prefix4.s_addr;
			req.dest.prefixlen = p->prefixlen;
			req.vrf_id = vrf_id;

			if(ng->nexthop)
				if (ng->nexthop->type == NEXTHOP_TYPE_IFINDEX)
					req.nh = req.dest.ip;
				else
					req.nh = ng->nexthop->gate.ipv4.s_addr;

			int ret = gr_api_client_send_recv(grout_ctx.client, GR_IP4_ROUTE_ADD, sizeof(req), &req, NULL);
			if (ret < 0 && ret != -EEXIST)
				return ZEBRA_DPLANE_REQUEST_FAILURE;
			else
				return ZEBRA_DPLANE_REQUEST_SUCCESS;
		}
	} else {
		if (p->family == AF_INET) {
			struct gr_ip4_route_del_req req = {.missing_ok = true};
			req.dest.ip = p->u.prefix4.s_addr;
			req.dest.prefixlen = p->prefixlen;
			req.vrf_id = vrf_id;

			if (gr_api_client_send_recv(grout_ctx.client, GR_IP4_ROUTE_DEL, sizeof(req), &req, NULL)  < 0)
				return ZEBRA_DPLANE_REQUEST_FAILURE;
			else
				return ZEBRA_DPLANE_REQUEST_SUCCESS;
		}
	}

	return ZEBRA_DPLANE_REQUEST_FAILURE;
}

/* Grout provider callback.
 */
static enum zebra_dplane_result zd_grout_process_update(struct zebra_dplane_ctx *ctx)
{
	switch (dplane_ctx_get_op(ctx)) {
	case DPLANE_OP_ADDR_INSTALL:
	case DPLANE_OP_ADDR_UNINSTALL:
	case DPLANE_OP_INTF_ADDR_ADD:
	case DPLANE_OP_INTF_ADDR_DEL:
		return zd_grout_add_del_address(ctx);
	case DPLANE_OP_SYS_ROUTE_ADD:
	case DPLANE_OP_SYS_ROUTE_DELETE:
	case DPLANE_OP_ROUTE_INSTALL:
	case DPLANE_OP_ROUTE_UPDATE:
	case DPLANE_OP_ROUTE_DELETE:
		return zd_grout_add_del_route(ctx);
	case DPLANE_OP_NH_INSTALL:
	case DPLANE_OP_NH_UPDATE:
	case DPLANE_OP_NH_DELETE:
		return zd_grout_add_del_nexthop(ctx);
	case DPLANE_OP_ROUTE_NOTIFY:
	case DPLANE_OP_RULE_ADD:
	case DPLANE_OP_RULE_UPDATE:
	case DPLANE_OP_RULE_DELETE:
	case DPLANE_OP_NONE:
	case DPLANE_OP_LSP_INSTALL:
	case DPLANE_OP_LSP_UPDATE:
	case DPLANE_OP_LSP_DELETE:
	case DPLANE_OP_LSP_NOTIFY:
	case DPLANE_OP_PW_INSTALL:
	case DPLANE_OP_PW_UNINSTALL:
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
	case DPLANE_OP_INTF_NETCONFIG:
	case DPLANE_OP_INTF_INSTALL:
	case DPLANE_OP_INTF_UPDATE:
	case DPLANE_OP_INTF_DELETE:
	case DPLANE_OP_VLAN_INSTALL:
	default:
		return ZEBRA_DPLANE_REQUEST_FAILURE;
	}
}

static int zd_grout_process(struct zebra_dplane_provider *prov)
{
	struct zebra_dplane_ctx *ctx;
	enum zebra_dplane_result ret;
	int counter, limit;

	limit = dplane_provider_get_work_limit(prov);

	for (counter = 0; counter < limit; counter++) {
		ctx = dplane_provider_dequeue_in_ctx(prov);
		if (ctx == NULL)
			break;

		if (IS_ZEBRA_DEBUG_DPLANE_GROUT_DETAIL)
			zlog_debug("dplane provider '%s': op %s",
				   dplane_provider_get_name(prov),
				   dplane_op2str(dplane_ctx_get_op(ctx)));

		ret = zd_grout_process_update(ctx);
		dplane_ctx_set_status(ctx, ret);
		if (ret == ZEBRA_DPLANE_REQUEST_SUCCESS)
			dplane_ctx_set_skip_kernel(ctx);

		dplane_provider_enqueue_out_ctx(prov, ctx);
	}

	if (IS_ZEBRA_DEBUG_DPLANE_GROUT_DETAIL)
		zlog_debug("dplane provider '%s': processed %d",
			   dplane_provider_get_name(prov), counter);
	/* Ensure that we'll run the work loop again if there's still
	 * more work to do.
	 */
	if (counter >= limit) {
		dplane_provider_work_ready();
	}

	return 0;
}

void zd_grout_port_show(struct vty *vty, uint16_t port_id, bool uj, int detail)
{
	// JSON
	if (uj) 
		return;

	if (!detail) {
		vty_out(vty, "%-4s %-16s %-16s %-16s %s\n", "Port", "Device",
			"IfName", "IfIndex", "devargs");
	}
}

static void zd_grout_port_init(void)
{
	struct gr_infra_iface_list_req req = {.type = GR_IFACE_TYPE_UNDEF};
	struct gr_infra_iface_list_resp *resp = NULL;
	struct gr_ip4_addr_list_resp *resp_ip4 = NULL;
	struct gr_ip6_addr_list_resp *resp_ip6 = NULL;
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
		goto cleanup;
	}

        if (gr_api_client_send_recv(grout_ctx.client, GR_IP4_ADDR_LIST, sizeof(req), &req, &resp_ptr) < 0) {
		if (IS_ZEBRA_DEBUG_DPLANE_GROUT)
			zlog_debug("Error listing ip4 addresses");
		goto cleanup;
	}
	resp_ip4 = resp_ptr;

        if (gr_api_client_send_recv(grout_ctx.client, GR_IP6_ADDR_LIST, sizeof(req), &req, &resp_ptr) < 0) {
		if (IS_ZEBRA_DEBUG_DPLANE_GROUT)
			zlog_debug("Error listing ip6 addresses");
		goto cleanup;
	}
	resp_ip6 = resp_ptr;

	for (int i = 0; i < resp->n_ifaces; i++) {
		struct interface *iface = if_get_by_name(resp->ifaces[i].name, resp->ifaces[i].vrf_id, NULL);
		if(iface == NULL)
			continue;

		if_set_index(iface, resp->ifaces[i].id +1000);
		iface->status = ZEBRA_INTERFACE_ACTIVE | ZEBRA_INTERFACE_LINKDETECTION;

		if (resp->ifaces[i].flags & GR_IFACE_F_UP)
			iface->flags |= IFF_UP;
		if (resp->ifaces[i].flags & GR_IFACE_F_PROMISC)
			iface->flags |= IFF_PROMISC;
		if (resp->ifaces[i].flags & GR_IFACE_F_ALLMULTI)
			iface->flags |= IFF_ALLMULTI;
		if (resp->ifaces[i].state & GR_IFACE_S_RUNNING)
			iface->flags |= IFF_RUNNING | IFF_LOWER_UP;

		// Force BROADCAST and MULTICAST
		iface->flags |= IFF_BROADCAST | IFF_MULTICAST;

		iface->speed = 25000;
		// TODO: read metric from grout ?
		iface->metric = 100;
		iface->mtu = resp->ifaces[i].mtu;
		iface->configured = true;
		if (resp->ifaces[i].type == GR_IFACE_TYPE_PORT) {
			memcpy(iface->hw_addr, &((struct gr_iface_info_port *)resp->ifaces[i].info)->mac, 6);
			iface->hw_addr_len = 6;
			iface->ll_type = ZEBRA_LLT_ETHER;
		} else if (resp->ifaces[i].type == GR_IFACE_TYPE_VLAN) {
			const struct gr_iface_info_vlan *vlan = (const struct gr_iface_info_vlan *)&resp->ifaces[i].info;

		        struct zebra_if *zif = (struct zebra_if *)iface->info;
	                struct zebra_l2info_vlan *vlan_info = &zif->l2info.vl;
			zif->zif_type = ZEBRA_IF_VLAN;
			zif->zif_slave_type = ZEBRA_IF_SLAVE_NONE;

			vlan_info->vid = vlan->vlan_id;

			memcpy(iface->hw_addr, &((struct gr_iface_info_vlan *)resp->ifaces[i].info)->mac, 6);
			iface->hw_addr_len = 6;
			iface->ll_type = ZEBRA_LLT_ETHER;

			// Cheat for vlan interfaces, force it to be "running",
			// Until https://github.com/DPDK/grout/issues/94 is fixed

			iface->flags |= IFF_RUNNING | IFF_LOWER_UP;
		}

		for (size_t j = 0; j < resp_ip4->n_addrs; j++) {
                	const struct gr_ip4_ifaddr *addr = &resp_ip4->addrs[j];
			struct in_addr sin_addr;
			uint8_t flags;

			if (addr->iface_id == resp->ifaces[i].id) {
				sin_addr.s_addr = addr->addr.ip;
				connected_add_ipv4(iface, flags, &sin_addr, addr->addr.prefixlen, NULL,
						NULL, 100);
				iface->flags |= IFF_IPV4;
			}
		}

		for (size_t j = 0; j < resp_ip6->n_addrs; j++) {
                	const struct gr_ip6_ifaddr *addr = &resp_ip6->addrs[j];
			struct in6_addr sin_addr;
			uint8_t flags;

			if (addr->iface_id == resp->ifaces[i].id) {
				memcpy(sin_addr.s6_addr, addr->addr.ip.a, sizeof(sin_addr.s6_addr));
				connected_add_ipv6(iface, flags, &sin_addr, NULL, addr->addr.prefixlen,
						NULL, 100);
				iface->flags |= IFF_IPV6;
			}
		}
	}
cleanup:
	free(resp);
	free(resp_ip4);
	free(resp_ip6);
}

static int zd_grout_start(struct zebra_dplane_provider *prov)
{
	struct frr_pthread_attr pattr = {
		.start = frr_pthread_attr_default.start,
		.stop = frr_pthread_attr_default.stop
	};

	grout_ctx.dg_run = true;
	grout_ctx.dg_pthread = frr_pthread_new(&pattr, "Zebra grout dplane thread",
						  "zebra_grout_dplane");
	event_add_event(grout_ctx.dg_pthread->master, dplane_read_notifications, NULL, 0,
			&grout_ctx.dg_t_update);

	frr_pthread_run(grout_ctx.dg_pthread, NULL);

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
	if (early) {
		frr_pthread_stop(grout_ctx.dg_pthread, NULL);
		return 0;
	}

	/* Destroy pthread */
	frr_pthread_destroy(grout_ctx.dg_pthread);

	gr_api_client_disconnect(grout_ctx.client);
	gr_api_client_disconnect(grout_ctx.notifs);
	return 0;
}

static int zd_grout_plugin_init(struct event_loop *tm)
{
	int ret;
	grout_ctx.client = gr_api_client_connect(GR_DEFAULT_SOCK_PATH);
	// grout_ctx.notifs = gr_api_client_connect(GR_DEFAULT_SOCK_PATH);

	ret = dplane_provider_register(plugin_name,
			DPLANE_PRIO_PRE_KERNEL,
			DPLANE_PROV_FLAGS_DEFAULT, // DPLANE_PROV_FLAG_THREADED,
			zd_grout_start,
			zd_grout_process,
			zd_grout_finish,
			&grout_ctx, NULL);

	if (ret != 0)
		zlog_err("Unable to register grout dplane provider: %d",
			 ret);

	if (IS_ZEBRA_DEBUG_DPLANE_GROUT)
		zlog_debug("%s register status %d", plugin_name, ret);

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