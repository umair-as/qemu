/* "Unimplemented" device
 *
 * This is a dummy device which accepts and logs all accesses.
 * It's useful for stubbing out regions of an SoC or board
 * map which correspond to devices that have not yet been
 * implemented. This is often sufficient to placate initial
 * guest device driver probing such that the system will
 * come up.
 *
 * With the "retain-writes" property set, reads return what was last
 * written (zero after reset), for drivers that read back a value they
 * wrote. Every access is still logged.
 *
 * Copyright Linaro Limited, 2017
 * Written by Peter Maydell
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/misc/unimp.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"

static uint64_t unimp_read(void *opaque, hwaddr offset, unsigned size)
{
    UnimplementedDeviceState *s = UNIMPLEMENTED_DEVICE(opaque);

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented device read  "
                  "(size %d, offset 0x%0*" HWADDR_PRIx ")\n",
                  s->name, size, s->offset_fmt_width, offset);
    if (s->retained) {
        return ldn_le_p(s->retained + offset, size);
    }
    return 0;
}

static void unimp_write(void *opaque, hwaddr offset,
                        uint64_t value, unsigned size)
{
    UnimplementedDeviceState *s = UNIMPLEMENTED_DEVICE(opaque);

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented device write "
                  "(size %d, offset 0x%0*" HWADDR_PRIx
                  ", value 0x%0*" PRIx64 ")\n",
                  s->name, size, s->offset_fmt_width, offset, size << 1, value);
    if (s->retained) {
        stn_le_p(s->retained + offset, size, value);
    }
}

static const MemoryRegionOps unimp_ops = {
    .read = unimp_read,
    .write = unimp_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void unimp_realize(DeviceState *dev, Error **errp)
{
    UnimplementedDeviceState *s = UNIMPLEMENTED_DEVICE(dev);

    if (s->size == 0) {
        error_setg(errp, "property 'size' not specified or zero");
        return;
    }

    if (s->name == NULL) {
        error_setg(errp, "property 'name' not specified");
        return;
    }

    s->offset_fmt_width = DIV_ROUND_UP(64 - clz64(s->size - 1), 4);

    if (s->retain_writes) {
        s->retained = g_malloc0(s->size);
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &unimp_ops, s,
                          s->name, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const Property unimp_properties[] = {
    DEFINE_PROP_UINT64("size", UnimplementedDeviceState, size, 0),
    DEFINE_PROP_STRING("name", UnimplementedDeviceState, name),
    DEFINE_PROP_BOOL("retain-writes", UnimplementedDeviceState,
                     retain_writes, false),
};

static void unimp_reset_hold(Object *obj, ResetType type)
{
    UnimplementedDeviceState *s = UNIMPLEMENTED_DEVICE(obj);

    if (s->retained) {
        memset(s->retained, 0, s->size);
    }
}

static void unimp_finalize(Object *obj)
{
    UnimplementedDeviceState *s = UNIMPLEMENTED_DEVICE(obj);

    g_free(s->retained);
}

static void unimp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = unimp_realize;
    rc->phases.hold = unimp_reset_hold;
    device_class_set_props(dc, unimp_properties);
}

static const TypeInfo unimp_info = {
    .name = TYPE_UNIMPLEMENTED_DEVICE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(UnimplementedDeviceState),
    .instance_finalize = unimp_finalize,
    .class_init = unimp_class_init,
};

static void unimp_register_types(void)
{
    type_register_static(&unimp_info);
}

type_init(unimp_register_types)
