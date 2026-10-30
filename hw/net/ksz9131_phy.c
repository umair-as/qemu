/*
 * Microchip KSZ9131 Gigabit Ethernet PHY
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The clause 22 and MMD registers that the Linux micrel driver and
 * phylib use on an RGMII KSZ9131, with an always-ready link partner
 * that advertises 10/100/1000 full and half duplex and pause.
 *
 * - BMCR reset and auto-negotiation restart complete at once; a link
 *   comes up negotiated as soon as a cable is attached and the PHY is
 *   not powered down. BMSR link status is latched low (IEEE 802.3
 *   22.2.4.2.13): a link loss reads 0 once, even if the link is back.
 * - Register 0x1B: bits 15:8 enable the interrupt of status bits 7:0;
 *   the status bits latch link up (bit 0) and link down (bit 2) and
 *   clear on read. INT_N is asserted while an enabled status bit is
 *   set, active low unless register 0x1F bit 14 selects active high.
 * - MMD registers through registers 0x0D/0x0E: the pad skew and DLL
 *   registers of device 2 hold what is written; the EEE registers of
 *   devices 3 and 7 report no EEE from the link partner.
 *
 * Not modelled: 10/100 forced modes (the link stays up at the
 * negotiated speed), loopback, cable diagnostics (register 0x12),
 * counters and the LED and other vendor registers. These read 0 and
 * are logged; writes to them are logged and ignored. The data path is
 * not affected by the skews, the speed or the duplex.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/net/mii.h"
#include "hw/net/ksz9131_phy.h"
#include "migration/vmstate.h"
#include "trace.h"

#define KSZ9131_PHYID1          0x0022
#define KSZ9131_PHYID2          0x1640  /* revision 0: not measured */

#define KSZ_DIG_AXAN_STS        0x14
#define KSZ_DIG_AXAN_LINK_DET   (1 << 14)
#define KSZ_DIG_AXAN_A_SELECT   (1 << 12)
#define KSZ_INT_CTRL_STATUS     0x1b
#define KSZ_INT_LINK_DOWN       (1 << 2)
#define KSZ_INT_LINK_UP         (1 << 0)
#define KSZ_AUTO_MDIX           0x1c
#define KSZ_CTRL1E              0x1e
#define KSZ_CTRL1F              0x1f
#define KSZ_CTRL1F_INT_HIGH     (1 << 14)

#define BMCR_RW (MII_BMCR_LOOPBACK | MII_BMCR_SPEED100 | MII_BMCR_AUTOEN | \
                 MII_BMCR_PDOWN | MII_BMCR_ISOLATE | MII_BMCR_FD | \
                 MII_BMCR_SPEED1000)

/* The MMD registers the driver uses, and their values after reset */
static const struct {
    uint8_t devad;
    uint16_t reg;
    uint16_t reset;
    bool rw;
} ksz9131_mmd[KSZ9131_PHY_NUM_MMD] = {
    { 2, 0x00, 0x0000, false },     /* common control */
    { 2, 0x04, 0x0077, true },      /* control pad skew */
    { 2, 0x05, 0x7777, true },      /* RX data pad skew */
    { 2, 0x06, 0x7777, true },      /* TX data pad skew */
    { 2, 0x08, 0x03ef, true },      /* clock pad skew */
    { 2, 0x4c, 0x4000, true },      /* RXC DLL control */
    { 2, 0x4d, 0x4000, true },      /* TXC DLL control */
    { 3, 0x14, 0x0006, false },     /* PCS EEE ability */
    { 7, 0x3c, 0x0006, true },      /* EEE advertisement */
    { 7, 0x3d, 0x0000, false },     /* link partner EEE ability */
};

static int ksz9131_mmd_index(int devad, uint16_t reg)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(ksz9131_mmd); i++) {
        if (ksz9131_mmd[i].devad == devad && ksz9131_mmd[i].reg == reg) {
            return i;
        }
    }
    return -1;
}

static void ksz9131_update_irq(KSZ9131PhyState *s)
{
    bool active = s->int_status & (s->int_ctrl >> 8);
    bool high = s->ctrl1f & KSZ_CTRL1F_INT_HIGH;

    qemu_set_irq(s->int_n, active == high);
}

/* The link follows the cable and the power down bit */
static void ksz9131_update_link(KSZ9131PhyState *s)
{
    bool link = s->cable && !(s->bmcr & MII_BMCR_PDOWN);

    if (link == s->link) {
        return;
    }
    s->link = link;
    trace_ksz9131_phy_link(link);
    if (link) {
        s->int_status |= KSZ_INT_LINK_UP;
    } else {
        s->int_status |= KSZ_INT_LINK_DOWN;
        s->lstatus_latched = true;
    }
    qemu_set_irq(s->link_out, link);
    ksz9131_update_irq(s);
}

static bool ksz9131_an_complete(KSZ9131PhyState *s)
{
    return s->link && (s->bmcr & MII_BMCR_AUTOEN);
}

static void ksz9131_soft_reset(KSZ9131PhyState *s)
{
    int i;

    s->bmcr = MII_BMCR_AUTOEN | MII_BMCR_FD | MII_BMCR_SPEED1000;
    s->anar = MII_ANAR_PAUSE_ASYM | MII_ANAR_PAUSE | MII_ANAR_TXFD |
              MII_ANAR_TX | MII_ANAR_10FD | MII_ANAR_10 | MII_ANAR_CSMACD;
    s->ctrl1000 = MII_CTRL1000_FULL | MII_CTRL1000_HALF;
    s->mmd_ctrl = 0;
    memset(s->mmd_addr, 0, sizeof(s->mmd_addr));
    s->int_ctrl = 0;
    s->int_status = 0;
    s->mdix = 0;
    s->ctrl1e = 0;
    s->ctrl1f = 0;
    for (i = 0; i < ARRAY_SIZE(ksz9131_mmd); i++) {
        s->mmd[i] = ksz9131_mmd[i].reset;
    }
}

static uint16_t ksz9131_mmd_access(KSZ9131PhyState *s, bool write,
                                   uint16_t val)
{
    int devad = s->mmd_ctrl & 0x1f;
    unsigned func = s->mmd_ctrl >> 14;
    uint16_t reg = s->mmd_addr[devad];
    int i;

    if (func == 0) {
        if (write) {
            s->mmd_addr[devad] = val;
        }
        return s->mmd_addr[devad];
    }
    /* 01 no increment, 10 increment on read and write, 11 on write */
    if (func == 2 || (func == 3 && write)) {
        s->mmd_addr[devad]++;
    }
    i = ksz9131_mmd_index(devad, reg);
    trace_ksz9131_phy_mmd(write ? "write" : "read", devad, reg, val);
    if (i < 0) {
        qemu_log_mask(LOG_UNIMP, "ksz9131: MMD %d register 0x%x %s\n",
                      devad, reg, write ? "write ignored" : "read as 0");
        return 0;
    }
    if (write && ksz9131_mmd[i].rw) {
        s->mmd[i] = val;
    }
    return s->mmd[i];
}

uint16_t ksz9131_phy_read(KSZ9131PhyState *s, int reg)
{
    bool an = ksz9131_an_complete(s);
    uint16_t val;

    switch (reg) {
    case MII_BMCR:
        val = s->bmcr;
        break;
    case MII_BMSR:
        val = MII_BMSR_100TX_FD | MII_BMSR_100TX_HD | MII_BMSR_10T_FD |
              MII_BMSR_10T_HD | MII_BMSR_EXTSTAT | MII_BMSR_MFPS |
              MII_BMSR_AUTONEG | MII_BMSR_EXTCAP;
        if (s->link && !s->lstatus_latched) {
            val |= MII_BMSR_LINK_ST;
        }
        if (an) {
            val |= MII_BMSR_AN_COMP;
        }
        s->lstatus_latched = false;
        break;
    case MII_PHYID1:
        val = KSZ9131_PHYID1;
        break;
    case MII_PHYID2:
        val = KSZ9131_PHYID2;
        break;
    case MII_ANAR:
        val = s->anar;
        break;
    case MII_ANLPAR:
        val = an ? (MII_ANLPAR_ACK | MII_ANLPAR_PAUSEASY | MII_ANLPAR_PAUSE |
                    MII_ANLPAR_TXFD | MII_ANLPAR_TX | MII_ANLPAR_10FD |
                    MII_ANLPAR_10 | MII_ANLPAR_CSMACD) : 0;
        break;
    case MII_CTRL1000:
        val = s->ctrl1000;
        break;
    case MII_STAT1000:
        val = an ? (MII_STAT1000_LOK | MII_STAT1000_ROK |
                    MII_STAT1000_FULL | MII_STAT1000_HALF) : 0;
        break;
    case MII_MDDACR:
        val = s->mmd_ctrl;
        break;
    case MII_MDDAADR:
        val = ksz9131_mmd_access(s, false, 0);
        break;
    case MII_EXTSTAT:
        val = MII_EXTSTAT_1000T_FD | MII_EXTSTAT_1000T_HD;
        break;
    case KSZ_DIG_AXAN_STS:
        val = s->link ? (KSZ_DIG_AXAN_LINK_DET | KSZ_DIG_AXAN_A_SELECT) : 0;
        break;
    case KSZ_INT_CTRL_STATUS:
        val = s->int_ctrl | s->int_status;
        s->int_status = 0;
        ksz9131_update_irq(s);
        break;
    case KSZ_AUTO_MDIX:
        val = s->mdix;
        break;
    case KSZ_CTRL1E:
        val = s->ctrl1e;
        break;
    case KSZ_CTRL1F:
        val = s->ctrl1f;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ksz9131: register 0x%x read as 0\n", reg);
        val = 0;
        break;
    }
    trace_ksz9131_phy_read(reg, val);
    return val;
}

void ksz9131_phy_write(KSZ9131PhyState *s, int reg, uint16_t val)
{
    trace_ksz9131_phy_write(reg, val);
    switch (reg) {
    case MII_BMCR:
        if (val & MII_BMCR_RESET) {
            /* Completes at once: the reset bit reads 0 again */
            ksz9131_soft_reset(s);
            ksz9131_update_irq(s);
        } else {
            /* A restart completes at once: the bit reads 0 again */
            s->bmcr = val & BMCR_RW;
        }
        ksz9131_update_link(s);
        break;
    case MII_ANAR:
        s->anar = val & ~MII_ANLPAR_ACK;
        break;
    case MII_CTRL1000:
        s->ctrl1000 = val & 0x1f00;
        break;
    case MII_MDDACR:
        s->mmd_ctrl = val & 0xc01f;
        break;
    case MII_MDDAADR:
        ksz9131_mmd_access(s, true, val);
        break;
    case KSZ_INT_CTRL_STATUS:
        s->int_ctrl = val & 0xff00;
        ksz9131_update_irq(s);
        break;
    case KSZ_AUTO_MDIX:
        s->mdix = val;
        break;
    case KSZ_CTRL1E:
        s->ctrl1e = val;
        break;
    case KSZ_CTRL1F:
        s->ctrl1f = val;
        ksz9131_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ksz9131: write 0x%04x to register 0x%x "
                      "ignored\n", val, reg);
        break;
    }
}

void ksz9131_phy_set_cable(KSZ9131PhyState *s, bool cable)
{
    s->cable = cable;
    ksz9131_update_link(s);
}

static void ksz9131_phy_reset_hold(Object *obj, ResetType type)
{
    KSZ9131PhyState *s = KSZ9131_PHY(obj);

    ksz9131_soft_reset(s);
    s->link = false;
    s->lstatus_latched = false;
}

static void ksz9131_phy_reset_exit(Object *obj, ResetType type)
{
    KSZ9131PhyState *s = KSZ9131_PHY(obj);

    /* A link that comes up after the reset is an interrupt event */
    qemu_set_irq(s->link_out, false);
    ksz9131_update_link(s);
    s->int_status = 0;
    ksz9131_update_irq(s);
}

static void ksz9131_phy_init(Object *obj)
{
    KSZ9131PhyState *s = KSZ9131_PHY(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->int_n);
    qdev_init_gpio_out_named(DEVICE(obj), &s->link_out, "link", 1);
}

static const VMStateDescription vmstate_ksz9131_phy = {
    .name = TYPE_KSZ9131_PHY,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(cable, KSZ9131PhyState),
        VMSTATE_BOOL(link, KSZ9131PhyState),
        VMSTATE_BOOL(lstatus_latched, KSZ9131PhyState),
        VMSTATE_UINT16(bmcr, KSZ9131PhyState),
        VMSTATE_UINT16(anar, KSZ9131PhyState),
        VMSTATE_UINT16(ctrl1000, KSZ9131PhyState),
        VMSTATE_UINT16(mmd_ctrl, KSZ9131PhyState),
        VMSTATE_UINT16_ARRAY(mmd_addr, KSZ9131PhyState, 32),
        VMSTATE_UINT16(int_ctrl, KSZ9131PhyState),
        VMSTATE_UINT16(int_status, KSZ9131PhyState),
        VMSTATE_UINT16(mdix, KSZ9131PhyState),
        VMSTATE_UINT16(ctrl1e, KSZ9131PhyState),
        VMSTATE_UINT16(ctrl1f, KSZ9131PhyState),
        VMSTATE_UINT16_ARRAY(mmd, KSZ9131PhyState, KSZ9131_PHY_NUM_MMD),
        VMSTATE_END_OF_LIST()
    }
};

static void ksz9131_phy_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Microchip KSZ9131 Gigabit Ethernet PHY";
    dc->vmsd = &vmstate_ksz9131_phy;
    rc->phases.hold = ksz9131_phy_reset_hold;
    rc->phases.exit = ksz9131_phy_reset_exit;
}

static const TypeInfo ksz9131_phy_info = {
    .name = TYPE_KSZ9131_PHY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(KSZ9131PhyState),
    .instance_init = ksz9131_phy_init,
    .class_init = ksz9131_phy_class_init,
};

static void ksz9131_phy_register_types(void)
{
    type_register_static(&ksz9131_phy_info);
}

type_init(ksz9131_phy_register_types)
