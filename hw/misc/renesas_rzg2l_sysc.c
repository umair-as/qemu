/*
 * Renesas RZ/G2L family System Controller (SYSC)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Only the three identification registers are modelled: SYS_LSI_MODE,
 * SYS_LSI_DEVID and SYS_LSI_PRR (manual sections 6.3.42 to 6.3.44).
 * They are read-only and reflect pins latched at power-on reset, so
 * writes are ignored. The other SYSC registers have non-zero reset
 * values (manual Table 6.1), so reading them as zero would be wrong:
 * any other offset is answered with a decode error instead, which the
 * guest sees as an external abort. Only 32-bit accesses are accepted.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/renesas_rzg2l_sysc.h"

REG32(SYS_LSI_MODE, 0xa00)
    FIELD(SYS_LSI_MODE, STAT_MD_BOOT, 0, 3)
    FIELD(SYS_LSI_MODE, STAT_DEBUGEN, 9, 1)
    FIELD(SYS_LSI_MODE, STAT_MD_CLKS, 12, 1)
    FIELD(SYS_LSI_MODE, STAT_MD_OSCDRV, 14, 2)
    FIELD(SYS_LSI_MODE, STAT_SEC_EN, 16, 1)
REG32(SYS_LSI_DEVID, 0xa04)
REG32(SYS_LSI_PRR, 0xa08)

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
    trace_renesas_rzg2l_sysc_write(offset, value, size);

    switch (offset) {
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

static void rzg2l_sysc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    /*
     * This device has no guest-modifiable state and so it
     * does not need a reset function or VMState.
     */
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
