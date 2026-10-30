/*
 * Arm CoreLink TZC-400 TrustZone Address Space Controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The programming interface that boot firmware uses (CoreLink TZC-400
 * TRM, chapter 3): the build configuration, the action register, the
 * gate keeper and the nine region register sets. Programming is stored,
 * not enforced: the model does not filter any transaction, so it never
 * reports a failure (INT_STATUS reads 0) and a filter's gate opens or
 * closes as soon as it is requested.
 *
 * Not modelled: the failure registers, the integration test registers
 * and the identification registers. An access to them, or to any other
 * offset, is logged and answered with a decode error.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "trace.h"
#include "hw/core/sysbus.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/misc/arm_tzc400.h"

REG32(BUILD_CONFIG, 0x000)
    FIELD(BUILD_CONFIG, NO_OF_REGIONS, 0, 5)
    FIELD(BUILD_CONFIG, ADDRESS_WIDTH, 8, 6)
    FIELD(BUILD_CONFIG, NO_OF_FILTERS, 24, 2)
REG32(ACTION, 0x004)
    FIELD(ACTION, REACTION_VALUE, 0, 2)
REG32(GATE_KEEPER, 0x008)
    FIELD(GATE_KEEPER, OPENREQ, 0, 4)
    FIELD(GATE_KEEPER, OPENSTAT, 16, 4)
REG32(SPECULATION_CTRL, 0x00c)
    FIELD(SPECULATION_CTRL, READ_SPEC_DISABLE, 0, 1)
    FIELD(SPECULATION_CTRL, WRITE_SPEC_DISABLE, 1, 1)
#define SPECULATION_CTRL_WRITABLE \
    (R_SPECULATION_CTRL_READ_SPEC_DISABLE_MASK | \
     R_SPECULATION_CTRL_WRITE_SPEC_DISABLE_MASK)
REG32(INT_STATUS, 0x010)
REG32(INT_CLEAR, 0x014)
REG32(REGION_BASE_LOW_0, 0x100)
#define REGION_STRIDE   0x20
#define REGION_END      (A_REGION_BASE_LOW_0 + \
                         ARM_TZC400_NUM_REGIONS * REGION_STRIDE)

enum {
    REGION_BASE_LOW,
    REGION_BASE_HIGH,
    REGION_TOP_LOW,
    REGION_TOP_HIGH,
    REGION_ATTRIBUTES,
    REGION_ID_ACCESS,
};

/* Region 0 covers the whole address space: its base and top are fixed */
static bool tzc400_region_fixed(int region, int reg)
{
    return region == 0 && reg <= REGION_TOP_HIGH;
}

static uint32_t tzc400_filter_mask(ArmTZC400State *s)
{
    return MAKE_64BIT_MASK(0, s->num_filters);
}

static uint64_t tzc400_region0_top(ArmTZC400State *s)
{
    return MAKE_64BIT_MASK(0, s->addr_width);
}

static MemTxResult arm_tzc400_read(void *opaque, hwaddr offset,
                                   uint64_t *data, unsigned size,
                                   MemTxAttrs attrs)
{
    ArmTZC400State *s = ARM_TZC400(opaque);
    uint64_t r;

    switch (offset) {
    case A_BUILD_CONFIG:
        r = 0;
        r = FIELD_DP32(r, BUILD_CONFIG, NO_OF_REGIONS,
                       ARM_TZC400_NUM_REGIONS - 1);
        r = FIELD_DP32(r, BUILD_CONFIG, ADDRESS_WIDTH, s->addr_width - 1);
        r = FIELD_DP32(r, BUILD_CONFIG, NO_OF_FILTERS, s->num_filters - 1);
        break;
    case A_ACTION:
        r = s->action;
        break;
    case A_GATE_KEEPER:
        /* Each filter's gate is as last requested */
        r = FIELD_DP32(s->gate_keeper, GATE_KEEPER, OPENSTAT,
                       FIELD_EX32(s->gate_keeper, GATE_KEEPER, OPENREQ));
        break;
    case A_SPECULATION_CTRL:
        r = s->speculation_ctrl;
        break;
    case A_INT_STATUS:
        r = 0;
        break;
    case A_REGION_BASE_LOW_0 ... REGION_END - 1: {
        int region = (offset - A_REGION_BASE_LOW_0) / REGION_STRIDE;
        int reg = (offset - A_REGION_BASE_LOW_0) % REGION_STRIDE / 4;

        if (reg >= ARM_TZC400_REGION_REGS) {
            goto unimp;
        }
        if (region == 0 && reg == REGION_TOP_LOW) {
            r = extract64(tzc400_region0_top(s), 0, 32);
        } else if (region == 0 && reg == REGION_TOP_HIGH) {
            r = extract64(tzc400_region0_top(s), 32, 32);
        } else {
            r = s->region[region][reg];
        }
        break;
    }
    default:
    unimp:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }

    trace_arm_tzc400_read(offset, r, size);
    *data = r;
    return MEMTX_OK;
}

static MemTxResult arm_tzc400_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    ArmTZC400State *s = ARM_TZC400(opaque);

    trace_arm_tzc400_write(offset, value, size);

    switch (offset) {
    case A_BUILD_CONFIG:
    case A_INT_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_OK;
    case A_ACTION:
        s->action = value & R_ACTION_REACTION_VALUE_MASK;
        return MEMTX_OK;
    case A_GATE_KEEPER:
        s->gate_keeper = value & tzc400_filter_mask(s);
        return MEMTX_OK;
    case A_SPECULATION_CTRL:
        s->speculation_ctrl = value & SPECULATION_CTRL_WRITABLE;
        return MEMTX_OK;
    case A_INT_CLEAR:
        /* No failure is ever recorded */
        return MEMTX_OK;
    case A_REGION_BASE_LOW_0 ... REGION_END - 1: {
        int region = (offset - A_REGION_BASE_LOW_0) / REGION_STRIDE;
        int reg = (offset - A_REGION_BASE_LOW_0) % REGION_STRIDE / 4;

        if (reg >= ARM_TZC400_REGION_REGS) {
            break;
        }
        if (tzc400_region_fixed(region, reg)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: write to region 0 base or top\n", __func__);
            return MEMTX_OK;
        }
        s->region[region][reg] = value;
        return MEMTX_OK;
    }
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented write at offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps arm_tzc400_ops = {
    .read_with_attrs = arm_tzc400_read,
    .write_with_attrs = arm_tzc400_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void arm_tzc400_reset_hold(Object *obj, ResetType type)
{
    ArmTZC400State *s = ARM_TZC400(obj);

    s->action = 0;
    s->gate_keeper = 0;
    s->speculation_ctrl = 0;
    memset(s->region, 0, sizeof(s->region));
}

static void arm_tzc400_init(Object *obj)
{
    ArmTZC400State *s = ARM_TZC400(obj);

    memory_region_init_io(&s->iomem, obj, &arm_tzc400_ops, s,
                          TYPE_ARM_TZC400, ARM_TZC400_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void arm_tzc400_realize(DeviceState *dev, Error **errp)
{
    ArmTZC400State *s = ARM_TZC400(dev);

    if (s->num_filters < 1 || s->num_filters > 4) {
        error_setg(errp, "num-filters must be 1-4");
        return;
    }
    if (s->addr_width < 32 || s->addr_width > 64) {
        error_setg(errp, "addr-width must be 32-64");
    }
}

static const Property arm_tzc400_props[] = {
    DEFINE_PROP_UINT8("num-filters", ArmTZC400State, num_filters, 0),
    DEFINE_PROP_UINT8("addr-width", ArmTZC400State, addr_width, 0),
};

static const VMStateDescription vmstate_arm_tzc400 = {
    .name = TYPE_ARM_TZC400,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(action, ArmTZC400State),
        VMSTATE_UINT32(gate_keeper, ArmTZC400State),
        VMSTATE_UINT32(speculation_ctrl, ArmTZC400State),
        VMSTATE_UINT32_2DARRAY(region, ArmTZC400State,
                               ARM_TZC400_NUM_REGIONS,
                               ARM_TZC400_REGION_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void arm_tzc400_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = arm_tzc400_reset_hold;
    dc->realize = arm_tzc400_realize;
    dc->vmsd = &vmstate_arm_tzc400;
    device_class_set_props(dc, arm_tzc400_props);
}

static const TypeInfo arm_tzc400_info = {
    .name = TYPE_ARM_TZC400,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArmTZC400State),
    .instance_init = arm_tzc400_init,
    .class_init = arm_tzc400_class_init,
};

static void arm_tzc400_register_types(void)
{
    type_register_static(&arm_tzc400_info);
}

type_init(arm_tzc400_register_types)
