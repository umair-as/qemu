/*
 * Renesas RZ/G2L family System Controller (SYSC)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Modelled (manual section 6.3):
 *  - the identification registers SYS_LSI_MODE, SYS_LSI_DEVID and
 *    SYS_LSI_PRR (6.3.42 to 6.3.44). They are read-only and reflect pins
 *    latched at power-on reset, so writes are ignored.
 *  - the Cortex-A55 reset vectors SYS_CA55_CFG_RVALn/RVAHn (6.3.38 to
 *    6.3.41): each core starts there when the CPG releases it.
 *  - the Cortex-A55 sleep handshake with the Cortex-M33: SYS_LP_CTL1,
 *    SYS_LP_CTL2, SYS_LP_CTL5 and SYS_LP_CTL6 (6.3.54 to 6.3.57). A core's
 *    sleep request sets its request flag in SYS_LP_CTL5 when SYS_LP_CTL6
 *    does not mask it; a 0 write clears a flag. The flags' interrupt to
 *    the Cortex-M33 (SYS_LPM_INT) and the Cortex-M33's responses are not
 *    modelled: there is no Cortex-M33 software.
 * The other SYSC registers have non-zero reset values (manual Table 6.1),
 * so reading them as zero would be wrong: any other offset is answered
 * with a decode error instead, which the guest sees as an external
 * abort. Only 32-bit accesses are accepted.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/misc/renesas_rzg2l_sysc.h"

REG32(SYS_CA55_CFG_RVAL0, 0x858)
REG32(SYS_CA55_CFG_RVAH0, 0x85c)
REG32(SYS_CA55_CFG_RVAL1, 0x860)
REG32(SYS_CA55_CFG_RVAH1, 0x864)
#define RVAL_MASK   0xfffffffc
#define RVAH_MASK   0x000000ff
REG32(SYS_LSI_MODE, 0xa00)
    FIELD(SYS_LSI_MODE, STAT_MD_BOOT, 0, 3)
    FIELD(SYS_LSI_MODE, STAT_DEBUGEN, 9, 1)
    FIELD(SYS_LSI_MODE, STAT_MD_CLKS, 12, 1)
    FIELD(SYS_LSI_MODE, STAT_MD_OSCDRV, 14, 2)
    FIELD(SYS_LSI_MODE, STAT_SEC_EN, 16, 1)
REG32(SYS_LSI_DEVID, 0xa04)
REG32(SYS_LSI_PRR, 0xa08)
REG32(SYS_LP_CTL1, 0xd04)
    FIELD(SYS_LP_CTL1, CA55SLEEP_REQ, 8, 2)
    FIELD(SYS_LP_CTL1, CM33SLEEP_REQ, 12, 1)
    FIELD(SYS_LP_CTL1, CA55SLEEP_ACK, 24, 2)
    FIELD(SYS_LP_CTL1, CM33SLEEP_ACK, 28, 1)
REG32(SYS_LP_CTL2, 0xd08)
    FIELD(SYS_LP_CTL2, CA55_STBYCTL, 0, 1)
REG32(SYS_LP_CTL5, 0xd14)
    FIELD(SYS_LP_CTL5, CA55SLEEP_F, 8, 2)
REG32(SYS_LP_CTL6, 0xd18)
    FIELD(SYS_LP_CTL6, CA55SLEEP_E, 8, 2)

#define LP_CTL1_WRITABLE (R_SYS_LP_CTL1_CA55SLEEP_REQ_MASK | \
                          R_SYS_LP_CTL1_CM33SLEEP_REQ_MASK | \
                          R_SYS_LP_CTL1_CA55SLEEP_ACK_MASK | \
                          R_SYS_LP_CTL1_CM33SLEEP_ACK_MASK)

/* Boot modes 0-5 are defined (manual Table 4.1); 6 and 7 are reserved */
#define RZG2L_SYSC_BOOT_MODE_MAX 5

static uint32_t rzg2l_sysc_lsi_mode(RenesasRZG2LSysCState *s)
{
    uint32_t r = 0;

    r = FIELD_DP32(r, SYS_LSI_MODE, STAT_MD_BOOT, s->boot_mode);
    r = FIELD_DP32(r, SYS_LSI_MODE, STAT_DEBUGEN, s->debugen);
    r = FIELD_DP32(r, SYS_LSI_MODE, STAT_MD_CLKS, s->md_clks);
    r = FIELD_DP32(r, SYS_LSI_MODE, STAT_MD_OSCDRV, s->md_oscdrv);
    r = FIELD_DP32(r, SYS_LSI_MODE, STAT_SEC_EN, s->sec_en);
    return r;
}

static MemTxResult rzg2l_sysc_read(void *opaque, hwaddr offset,
                                   uint64_t *data, unsigned size,
                                   MemTxAttrs attrs)
{
    RenesasRZG2LSysCState *s = RENESAS_RZG2L_SYSC(opaque);
    uint64_t r;

    switch (offset) {
    case A_SYS_LSI_MODE:
        r = rzg2l_sysc_lsi_mode(s);
        break;
    case A_SYS_LSI_DEVID:
        r = s->lsi_devid;
        break;
    case A_SYS_LSI_PRR:
        r = s->lsi_prr;
        break;
    case A_SYS_CA55_CFG_RVAL0:
    case A_SYS_CA55_CFG_RVAL1:
        r = extract64(s->rvba[(offset - A_SYS_CA55_CFG_RVAL0) / 8], 0, 32);
        break;
    case A_SYS_CA55_CFG_RVAH0:
    case A_SYS_CA55_CFG_RVAH1:
        r = extract64(s->rvba[(offset - A_SYS_CA55_CFG_RVAH0) / 8], 32, 32);
        break;
    case A_SYS_LP_CTL1:
        r = s->lp_ctl1;
        break;
    case A_SYS_LP_CTL2:
        r = s->lp_ctl2;
        break;
    case A_SYS_LP_CTL5:
        r = s->lp_ctl5;
        break;
    case A_SYS_LP_CTL6:
        r = s->lp_ctl6;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }

    trace_renesas_rzg2l_sysc_read(offset, r, size);
    *data = r;
    return MEMTX_OK;
}

static MemTxResult rzg2l_sysc_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    RenesasRZG2LSysCState *s = RENESAS_RZG2L_SYSC(opaque);
    uint32_t raised;
    int core;

    trace_renesas_rzg2l_sysc_write(offset, value, size);

    switch (offset) {
    case A_SYS_CA55_CFG_RVAL0:
    case A_SYS_CA55_CFG_RVAL1:
        core = (offset - A_SYS_CA55_CFG_RVAL0) / 8;
        s->rvba[core] = deposit64(s->rvba[core], 0, 32, value & RVAL_MASK);
        return MEMTX_OK;
    case A_SYS_CA55_CFG_RVAH0:
    case A_SYS_CA55_CFG_RVAH1:
        core = (offset - A_SYS_CA55_CFG_RVAH0) / 8;
        s->rvba[core] = deposit64(s->rvba[core], 32, 32, value & RVAH_MASK);
        return MEMTX_OK;
    case A_SYS_LP_CTL1:
        /* A new sleep request sets its flag, unless it is masked */
        raised = FIELD_EX32(value & ~s->lp_ctl1, SYS_LP_CTL1, CA55SLEEP_REQ) &
                 FIELD_EX32(s->lp_ctl6, SYS_LP_CTL6, CA55SLEEP_E);
        s->lp_ctl1 = value & LP_CTL1_WRITABLE;
        s->lp_ctl5 |= FIELD_DP32(0, SYS_LP_CTL5, CA55SLEEP_F, raised);
        return MEMTX_OK;
    case A_SYS_LP_CTL2:
        s->lp_ctl2 = value & R_SYS_LP_CTL2_CA55_STBYCTL_MASK;
        return MEMTX_OK;
    case A_SYS_LP_CTL5:
        /* The flags clear on a 0 write */
        s->lp_ctl5 &= value | ~R_SYS_LP_CTL5_CA55SLEEP_F_MASK;
        return MEMTX_OK;
    case A_SYS_LP_CTL6:
        s->lp_ctl6 = value & R_SYS_LP_CTL6_CA55SLEEP_E_MASK;
        return MEMTX_OK;
    case A_SYS_LSI_MODE:
    case A_SYS_LSI_DEVID:
    case A_SYS_LSI_PRR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }
}

static const MemoryRegionOps rzg2l_sysc_ops = {
    .read_with_attrs = rzg2l_sysc_read,
    .write_with_attrs = rzg2l_sysc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const Property rzg2l_sysc_props[] = {
    DEFINE_PROP_UINT8("boot-mode", RenesasRZG2LSysCState, boot_mode, 0),
    DEFINE_PROP_BOOL("debugen", RenesasRZG2LSysCState, debugen, false),
    DEFINE_PROP_BOOL("md-clks", RenesasRZG2LSysCState, md_clks, false),
    DEFINE_PROP_UINT8("md-oscdrv", RenesasRZG2LSysCState, md_oscdrv, 0),
    DEFINE_PROP_BOOL("sec-en", RenesasRZG2LSysCState, sec_en, false),
    DEFINE_PROP_UINT32("lsi-devid", RenesasRZG2LSysCState, lsi_devid, 0),
    DEFINE_PROP_UINT32("lsi-prr", RenesasRZG2LSysCState, lsi_prr, 0),
};

static void rzg2l_sysc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RenesasRZG2LSysCState *s = RENESAS_RZG2L_SYSC(obj);

    memory_region_init_io(&s->iomem, obj, &rzg2l_sysc_ops, s,
                          TYPE_RENESAS_RZG2L_SYSC,
                          RENESAS_RZG2L_SYSC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void rzg2l_sysc_realize(DeviceState *dev, Error **errp)
{
    RenesasRZG2LSysCState *s = RENESAS_RZG2L_SYSC(dev);

    if (s->boot_mode > RZG2L_SYSC_BOOT_MODE_MAX) {
        error_setg(errp, "boot-mode %u is reserved (valid: 0-%u)",
                   s->boot_mode, RZG2L_SYSC_BOOT_MODE_MAX);
        return;
    }
    if (s->md_oscdrv > 3) {
        error_setg(errp, "md-oscdrv %u does not fit MD_OSCDRV[1:0]",
                   s->md_oscdrv);
        return;
    }
    /* The device ID is per SoC; there is no meaningful default */
    if (!s->lsi_devid) {
        error_setg(errp, "lsi-devid property must be set");
        return;
    }
}

uint64_t rzg2l_sysc_reset_vector(RenesasRZG2LSysCState *s, int core)
{
    assert(core >= 0 && core < RENESAS_RZG2L_SYSC_NUM_CA55);
    return s->rvba[core];
}

static void rzg2l_sysc_reset_hold(Object *obj, ResetType type)
{
    RenesasRZG2LSysCState *s = RENESAS_RZG2L_SYSC(obj);

    memset(s->rvba, 0, sizeof(s->rvba));
    s->lp_ctl1 = 0;
    s->lp_ctl2 = 0;
    s->lp_ctl5 = 0;
    s->lp_ctl6 = 0;
}

static const VMStateDescription vmstate_rzg2l_sysc = {
    .name = TYPE_RENESAS_RZG2L_SYSC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(rvba, RenesasRZG2LSysCState,
                             RENESAS_RZG2L_SYSC_NUM_CA55),
        VMSTATE_UINT32(lp_ctl1, RenesasRZG2LSysCState),
        VMSTATE_UINT32(lp_ctl2, RenesasRZG2LSysCState),
        VMSTATE_UINT32(lp_ctl5, RenesasRZG2LSysCState),
        VMSTATE_UINT32(lp_ctl6, RenesasRZG2LSysCState),
        VMSTATE_END_OF_LIST()
    }
};

static void rzg2l_sysc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = rzg2l_sysc_reset_hold;
    dc->vmsd = &vmstate_rzg2l_sysc;
    dc->realize = rzg2l_sysc_realize;
    device_class_set_props(dc, rzg2l_sysc_props);
}

static const TypeInfo rzg2l_sysc_info = {
    .name = TYPE_RENESAS_RZG2L_SYSC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZG2LSysCState),
    .instance_init = rzg2l_sysc_init,
    .class_init = rzg2l_sysc_class_init,
};

static void rzg2l_sysc_register_types(void)
{
    type_register_static(&rzg2l_sysc_info);
}

type_init(rzg2l_sysc_register_types);
