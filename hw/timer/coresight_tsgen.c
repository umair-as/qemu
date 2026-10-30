/*
 * Arm CoreSight SoC-400 timestamp generator, control frame
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The system counter of SoCs that feed their Cortex-A cores' generic
 * timers from a CoreSight timestamp generator (CoreSight SoC-400 TRM,
 * section 3.10). Modelled: the control registers firmware programs at
 * boot, CNTCR (enable, halt on debug) and CNTFID0 (the counter frequency
 * that firmware copies into CNTFRQ_EL0), and CNTSR. Firmware writes
 * CNTFID0 itself; the model only stores it. With "direct-boot", the
 * registers start as firmware leaves them: CNTCR.EN set and CNTFID0 the
 * "direct-boot-freq" property.
 *
 * Not modelled: the count itself. The CPUs' generic timers count from
 * QEMU's virtual clock whether or not CNTCR.EN is set, and the counter
 * value registers (CNTCVL/CNTCVU in both frames) and the identification
 * registers are not implemented: an access to them, or to any other
 * offset, is logged and answered with a decode error.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/core/sysbus.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/coresight_tsgen.h"

REG32(CNTCR, 0x000)
    FIELD(CNTCR, EN, 0, 1)
    FIELD(CNTCR, HDBG, 1, 1)
REG32(CNTSR, 0x004)
REG32(CNTFID0, 0x020)

#define CNTCR_WRITABLE (R_CNTCR_EN_MASK | R_CNTCR_HDBG_MASK)

static MemTxResult coresight_tsgen_read(void *opaque, hwaddr offset,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    CoreSightTSGenState *s = CORESIGHT_TSGEN(opaque);
    uint64_t r;

    switch (offset) {
    case A_CNTCR:
        r = s->cntcr;
        break;
    case A_CNTSR:
        /* DBGH: the counter is never halted by a debug request */
        r = 0;
        break;
    case A_CNTFID0:
        r = s->cntfid0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }

    trace_coresight_tsgen_read(offset, r, size);
    *data = r;
    return MEMTX_OK;
}

static MemTxResult coresight_tsgen_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    CoreSightTSGenState *s = CORESIGHT_TSGEN(opaque);

    trace_coresight_tsgen_write(offset, value, size);

    switch (offset) {
    case A_CNTCR:
        s->cntcr = value & CNTCR_WRITABLE;
        return MEMTX_OK;
    case A_CNTSR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only CNTSR\n", __func__);
        return MEMTX_OK;
    case A_CNTFID0:
        s->cntfid0 = value;
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }
}

static const MemoryRegionOps coresight_tsgen_ops = {
    .read_with_attrs = coresight_tsgen_read,
    .write_with_attrs = coresight_tsgen_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void coresight_tsgen_reset_hold(Object *obj, ResetType type)
{
    CoreSightTSGenState *s = CORESIGHT_TSGEN(obj);

    s->cntcr = s->direct_boot ? R_CNTCR_EN_MASK : 0;
    s->cntfid0 = s->direct_boot ? s->direct_boot_freq : 0;
}

static const Property coresight_tsgen_props[] = {
    DEFINE_PROP_BOOL("direct-boot", CoreSightTSGenState, direct_boot, false),
    DEFINE_PROP_UINT32("direct-boot-freq", CoreSightTSGenState,
                       direct_boot_freq, 0),
};

static void coresight_tsgen_init(Object *obj)
{
    CoreSightTSGenState *s = CORESIGHT_TSGEN(obj);

    memory_region_init_io(&s->iomem, obj, &coresight_tsgen_ops, s,
                          TYPE_CORESIGHT_TSGEN, CORESIGHT_TSGEN_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_coresight_tsgen = {
    .name = TYPE_CORESIGHT_TSGEN,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cntcr, CoreSightTSGenState),
        VMSTATE_UINT32(cntfid0, CoreSightTSGenState),
        VMSTATE_END_OF_LIST()
    }
};

static void coresight_tsgen_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = coresight_tsgen_reset_hold;
    dc->vmsd = &vmstate_coresight_tsgen;
    device_class_set_props(dc, coresight_tsgen_props);
}

static const TypeInfo coresight_tsgen_info = {
    .name = TYPE_CORESIGHT_TSGEN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CoreSightTSGenState),
    .instance_init = coresight_tsgen_init,
    .class_init = coresight_tsgen_class_init,
};

static void coresight_tsgen_register_types(void)
{
    type_register_static(&coresight_tsgen_info);
}

type_init(coresight_tsgen_register_types)
