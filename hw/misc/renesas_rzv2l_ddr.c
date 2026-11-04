/*
 * Renesas RZ/V2L DDR memory controller and PHY (stub)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The RZ/V2L's DDR4 memory controller (registers at 0x11410000) and PHY
 * (0x11400000), as a stub for the boot firmware's DDR set-up. The
 * RZ/V2L Group User's Manual: Hardware documents neither block's
 * registers (section 9 lists features and pins; Table 5.1 the windows).
 * The stub's contract is what TF-A's BL2 (plat/renesas/rz/common/
 * drivers/ddr/ddr.c) writes, polls and checks; it is not a model of the
 * silicon.
 *
 * - Supported registers are the ones BL2 reads or writes: the
 *   controller's 0x000-0x7f8 except 0x070, 0x11c, 0x120 and 0x124, and
 *   51 PHY offsets. They store what is written and reset to 0, except:
 * - Controller: setting START (0x000 bit 0) sets the init-done status
 *   (0x260 bit 25) at once; clearing START or writing 1 to its
 *   acknowledgement (0x280 bit 25) clears it. A write to 0x114 with
 *   bit 25 set (a mode-register write) sets 0x264 bit 3 at once; 1 to
 *   0x284 bit 3 clears it. LP_CMD (0x0e8 bits 14:8) 0x11 sets LP_STATE
 *   (0x0f4 bits 30:24) to 0x48, 0x02 to 0x40; 0 does nothing. The status
 *   registers and the low-power status (0x24c bits 31:16) ignore writes;
 *   the acknowledgement registers read 0. ECC_ENABLE (0x18c) bits 11:0
 *   read 1, as the board reads them after the boot firmware.
 * - PHY: one of the seven training commands BL2 writes to SCL_START
 *   (0x100) completes at once: busy (bit 28) reads 0 and bits 1:0 read
 *   3; write leveling (0x50200000) also clears bits 1:0 of 0x14c. 0x148
 *   and 0x160 read 3 in bits 1:0, 0x1d4 reads 0; writes to these three
 *   are logged and ignored. 0x170 bits 3:2 read stored bits 1:0.
 * - An unsupported offset or access size, an unknown training command
 *   and an unknown low-power command are logged and refused with a bus
 *   error.
 * Not modelled: the training (the PHY keeps the values BL2 writes), the
 * PHY's lane banking, ECC, BIST, timing, refresh, the interrupts and the
 * clock controller's DDR resets.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/renesas_rzv2l_ddr.h"
#include "migration/vmstate.h"
#include "trace.h"

/* Controller */
#define MC_CTL00            0x000
#define MC_CTL00_START      BIT(0)
#define MC_LP_CMD           0x0e8
#define MC_LP_STATE         0x0f4
#define MC_MRW              0x114
#define MC_MRW_REQ          BIT(25)
#define MC_ECC_ENABLE       0x18c
#define MC_ST_LOWPOWER      0x24c
#define MC_ST_INIT          0x260
#define MC_ST_MODE          0x264
#define MC_ACK_LOWPOWER     0x26c
#define MC_ACK_BIST         0x27c
#define MC_ACK_INIT         0x280
#define MC_ACK_MODE         0x284
#define MC_INIT_DONE        BIT(25)
#define MC_MRW_DONE         BIT(3)
#define MC_LAST             0x7f8

/* PHY */
#define PHY_SCL_START       0x100
#define PHY_SCL_BUSY        BIT(28)
#define PHY_WRLVL_CTRL      0x148
#define PHY_WRLVL_TRIM      0x14c
#define PHY_DLL_2           0x160
#define PHY_IO_1            0x170
#define PHY_VREF_WINDOWS    0x1d4
#define PHY_LANES           0x3     /* two byte lanes */
#define PHY_CMD_WRLVL       0x50200000

static const uint16_t phy_offsets[] = {
    0x058, 0x060, 0x09c, 0x0a8, 0x0c0, 0x0c4, 0x0c8, 0x100, 0x104, 0x108,
    0x10c, 0x118, 0x11c, 0x120, 0x124, 0x128, 0x12c, 0x130, 0x134, 0x138,
    0x140, 0x148, 0x14c, 0x150, 0x154, 0x15c, 0x160, 0x164, 0x168, 0x16c,
    0x170, 0x174, 0x188, 0x194, 0x19c, 0x1a0, 0x1a4, 0x1ac, 0x1b4, 0x1b8,
    0x1bc, 0x1c0, 0x1c4, 0x1c8, 0x1cc, 0x1d4, 0x1e0, 0x1e8, 0x200, 0x204,
    0x240,
};

/* The training commands BL2 writes to SCL_START */
static const uint32_t phy_commands[] = {
    0x50200000, 0x30800000, 0x30500000, 0x30a00000, 0x30700000,
    0x11200000, 0x34200000,
};

static bool ddr_mc_supported(hwaddr offset)
{
    return offset <= MC_LAST && offset != 0x070 && offset != 0x11c &&
           offset != 0x120 && offset != 0x124;
}

static bool ddr_phy_supported(hwaddr offset)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(phy_offsets); i++) {
        if (phy_offsets[i] == offset) {
            return true;
        }
    }
    return false;
}

static bool ddr_access_ok(const char *block, hwaddr offset, unsigned size,
                          bool supported)
{
    if (size != 4 || (offset & 3) || !supported) {
        qemu_log_mask(LOG_UNIMP, "renesas-rzv2l-ddr: %u-byte %s access at "
                      "0x%" HWADDR_PRIx " not supported\n", size, block,
                      offset);
        return false;
    }
    return true;
}

static MemTxResult ddr_mc_read(void *opaque, hwaddr offset, uint64_t *data,
                               unsigned size, MemTxAttrs attrs)
{
    RenesasRZV2LDDRState *s = RENESAS_RZV2L_DDR(opaque);
    uint32_t v;

    if (!ddr_access_ok("controller", offset, size, ddr_mc_supported(offset))) {
        return MEMTX_ERROR;
    }
    switch (offset) {
    case MC_ST_INIT:
        v = s->init_done ? MC_INIT_DONE : 0;
        break;
    case MC_ST_MODE:
        v = s->mrw_done ? MC_MRW_DONE : 0;
        break;
    case MC_ST_LOWPOWER:
        v = s->mc[offset / 4] & 0x0000ffff;
        break;
    case MC_LP_STATE:
        v = deposit32(s->mc[offset / 4], 24, 7, s->lp_state);
        break;
    case MC_ECC_ENABLE:
        v = s->mc[offset / 4] | 0xfff;
        break;
    default:
        v = s->mc[offset / 4];
        break;
    }
    trace_renesas_rzv2l_ddr_mc_read(offset, v);
    *data = v;
    return MEMTX_OK;
}

static MemTxResult ddr_mc_write(void *opaque, hwaddr offset, uint64_t data,
                                unsigned size, MemTxAttrs attrs)
{
    RenesasRZV2LDDRState *s = RENESAS_RZV2L_DDR(opaque);
    uint32_t v = data;
    uint32_t old;
    unsigned cmd;

    if (!ddr_access_ok("controller", offset, size, ddr_mc_supported(offset))) {
        return MEMTX_ERROR;
    }
    trace_renesas_rzv2l_ddr_mc_write(offset, v);
    switch (offset) {
    case MC_CTL00:
        old = s->mc[offset / 4];
        s->mc[offset / 4] = v;
        if ((v & MC_CTL00_START) && !(old & MC_CTL00_START)) {
            s->init_done = true;
            trace_renesas_rzv2l_ddr_event("init done");
        } else if (!(v & MC_CTL00_START)) {
            s->init_done = false;
        }
        break;
    case MC_ST_INIT:
    case MC_ST_MODE:
        /* Read-only status: computed on read */
        break;
    case MC_ACK_INIT:
        if (v & MC_INIT_DONE) {
            s->init_done = false;
            trace_renesas_rzv2l_ddr_event("init done acknowledged");
        }
        break;
    case MC_ACK_MODE:
        if (v & MC_MRW_DONE) {
            s->mrw_done = false;
            trace_renesas_rzv2l_ddr_event("mode register write acknowledged");
        }
        break;
    case MC_ACK_LOWPOWER:
    case MC_ACK_BIST:
        /* Acknowledgements are not stored: they read 0 */
        break;
    case MC_MRW:
        s->mc[offset / 4] = v;
        if (v & MC_MRW_REQ) {
            s->mrw_done = true;
            trace_renesas_rzv2l_ddr_event("mode register write");
        }
        break;
    case MC_LP_CMD:
        cmd = extract32(v, 8, 7);
        if (cmd == 0x11) {
            s->lp_state = 0x48;
        } else if (cmd == 0x02) {
            s->lp_state = 0x40;
        } else if (cmd) {
            qemu_log_mask(LOG_UNIMP, "renesas-rzv2l-ddr: low-power command "
                          "0x%x not supported\n", cmd);
            return MEMTX_ERROR;
        }
        s->mc[offset / 4] = v;
        trace_renesas_rzv2l_ddr_lp(cmd, s->lp_state);
        break;
    default:
        s->mc[offset / 4] = v;
        break;
    }
    return MEMTX_OK;
}

static MemTxResult ddr_phy_read(void *opaque, hwaddr offset, uint64_t *data,
                                unsigned size, MemTxAttrs attrs)
{
    RenesasRZV2LDDRState *s = RENESAS_RZV2L_DDR(opaque);
    uint32_t v;

    if (!ddr_access_ok("PHY", offset, size, ddr_phy_supported(offset))) {
        return MEMTX_ERROR;
    }
    switch (offset) {
    case PHY_SCL_START:
        v = (s->phy[offset / 4] & ~PHY_SCL_BUSY) | PHY_LANES;
        break;
    case PHY_WRLVL_CTRL:
    case PHY_DLL_2:
        v = PHY_LANES;
        break;
    case PHY_IO_1:
        v = s->phy[offset / 4] | (extract32(s->phy[offset / 4], 0, 2) << 2);
        break;
    default:
        v = s->phy[offset / 4];
        break;
    }
    trace_renesas_rzv2l_ddr_phy_read(offset, v);
    *data = v;
    return MEMTX_OK;
}

static MemTxResult ddr_phy_write(void *opaque, hwaddr offset, uint64_t data,
                                 unsigned size, MemTxAttrs attrs)
{
    RenesasRZV2LDDRState *s = RENESAS_RZV2L_DDR(opaque);
    uint32_t v = data;
    int i;

    if (!ddr_access_ok("PHY", offset, size, ddr_phy_supported(offset))) {
        return MEMTX_ERROR;
    }
    trace_renesas_rzv2l_ddr_phy_write(offset, v);
    switch (offset) {
    case PHY_WRLVL_CTRL:
    case PHY_DLL_2:
    case PHY_VREF_WINDOWS:
        /* Read-only status: not stored, 0x1d4 reads its reset value 0 */
        qemu_log_mask(LOG_GUEST_ERROR, "renesas-rzv2l-ddr: write to the "
                      "PHY status register at 0x%" HWADDR_PRIx " ignored\n",
                      offset);
        break;
    case PHY_SCL_START:
        if (v & PHY_SCL_BUSY) {
            for (i = 0; i < ARRAY_SIZE(phy_commands); i++) {
                if (phy_commands[i] == v) {
                    break;
                }
            }
            if (i == ARRAY_SIZE(phy_commands)) {
                qemu_log_mask(LOG_UNIMP, "renesas-rzv2l-ddr: training "
                              "command 0x%08x not supported\n", v);
                return MEMTX_ERROR;
            }
            if (v == PHY_CMD_WRLVL) {
                s->phy[PHY_WRLVL_TRIM / 4] &= ~PHY_LANES;
            }
            trace_renesas_rzv2l_ddr_event("training command done");
        }
        s->phy[offset / 4] = v;
        break;
    case PHY_IO_1:
        /* Bits 3:2 are computed from bits 1:0 */
        s->phy[offset / 4] = v & ~0xc;
        break;
    default:
        s->phy[offset / 4] = v;
        break;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ddr_mc_ops = {
    .read_with_attrs = ddr_mc_read,
    .write_with_attrs = ddr_mc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps ddr_phy_ops = {
    .read_with_attrs = ddr_phy_read,
    .write_with_attrs = ddr_phy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void ddr_reset_hold(Object *obj, ResetType type)
{
    RenesasRZV2LDDRState *s = RENESAS_RZV2L_DDR(obj);

    memset(s->mc, 0, sizeof(s->mc));
    memset(s->phy, 0, sizeof(s->phy));
    s->init_done = false;
    s->mrw_done = false;
    s->lp_state = 0;
}

static void ddr_init(Object *obj)
{
    RenesasRZV2LDDRState *s = RENESAS_RZV2L_DDR(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mc_iomem, obj, &ddr_mc_ops, s,
                          "renesas-rzv2l-ddr.memc",
                          RENESAS_RZV2L_DDR_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mc_iomem);
    memory_region_init_io(&s->phy_iomem, obj, &ddr_phy_ops, s,
                          "renesas-rzv2l-ddr.phy",
                          RENESAS_RZV2L_DDR_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->phy_iomem);
}

static const VMStateDescription vmstate_ddr = {
    .name = TYPE_RENESAS_RZV2L_DDR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(mc, RenesasRZV2LDDRState,
                             RENESAS_RZV2L_DDR_MC_REGS),
        VMSTATE_UINT32_ARRAY(phy, RenesasRZV2LDDRState,
                             RENESAS_RZV2L_DDR_PHY_REGS),
        VMSTATE_BOOL(init_done, RenesasRZV2LDDRState),
        VMSTATE_BOOL(mrw_done, RenesasRZV2LDDRState),
        VMSTATE_UINT8(lp_state, RenesasRZV2LDDRState),
        VMSTATE_END_OF_LIST()
    },
};

static void ddr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Renesas RZ/V2L DDR memory controller and PHY (stub)";
    dc->vmsd = &vmstate_ddr;
    rc->phases.hold = ddr_reset_hold;
}

static const TypeInfo ddr_info = {
    .name = TYPE_RENESAS_RZV2L_DDR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZV2LDDRState),
    .instance_init = ddr_init,
    .class_init = ddr_class_init,
};

static void ddr_register_types(void)
{
    type_register_static(&ddr_info);
}

type_init(ddr_register_types)
