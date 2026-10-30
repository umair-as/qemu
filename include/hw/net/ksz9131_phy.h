/*
 * Microchip KSZ9131 Gigabit Ethernet PHY
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * QEMU interface:
 *  + sysbus IRQ 0: the INT_N pin level (high when idle; polarity per
 *    register 0x1F bit 14)
 *  + named GPIO output "link": high while the link is up
 *  + ksz9131_phy_read() / ksz9131_phy_write(): a clause 22 register
 *    access, as the MDIO frame decoder of the MAC delivers it
 *  + ksz9131_phy_set_cable(): whether a link partner is attached
 */

#ifndef HW_NET_KSZ9131_PHY_H
#define HW_NET_KSZ9131_PHY_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_KSZ9131_PHY "ksz9131-phy"
OBJECT_DECLARE_SIMPLE_TYPE(KSZ9131PhyState, KSZ9131_PHY)

#define KSZ9131_PHY_NUM_MMD 10

struct KSZ9131PhyState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    qemu_irq int_n;
    qemu_irq link_out;

    bool cable;
    bool link;
    bool lstatus_latched;
    uint16_t bmcr;
    uint16_t anar;
    uint16_t ctrl1000;
    uint16_t mmd_ctrl;
    uint16_t mmd_addr[32];
    uint16_t int_ctrl;
    uint16_t int_status;
    uint16_t mdix;
    uint16_t ctrl1e;
    uint16_t ctrl1f;
    uint16_t mmd[KSZ9131_PHY_NUM_MMD];
};

uint16_t ksz9131_phy_read(KSZ9131PhyState *s, int reg);
void ksz9131_phy_write(KSZ9131PhyState *s, int reg, uint16_t val);
void ksz9131_phy_set_cable(KSZ9131PhyState *s, bool cable);

#endif
