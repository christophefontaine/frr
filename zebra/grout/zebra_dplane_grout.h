// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Zebra dataplane plugin for Grout
 *
 * Copyright (C) 2024 Red Hat
 * Christophe Fontaine
 */

#ifndef _ZEBRA_DPLANE_GROUT_H
#define _ZEBRA_DPLANE_GROUT_H

#include <zebra.h>
#include <lib/vty.h>
#define ZD_GROUT_INVALID_PORT 0

void zd_grout_port_show(struct vty *vty, uint16_t port_id, bool uj, int detail);
void zd_grout_vty_init(void);

extern struct zebra_privs_t zserv_privs;

#endif
