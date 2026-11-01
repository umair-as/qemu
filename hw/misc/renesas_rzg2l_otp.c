/*
 * Renesas RZ/G2L family One-Time Programmable memory (OTP)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/renesas_rzg2l_otp.h"

/* Direct read offsets of OTP words 0x50-0x53, the CHIP Product ID */
#define OTP_CHIPID_OFFSET   0x1140
#define OTP_CHIPID_END      (OTP_CHIPID_OFFSET + \
                             4 * RENESAS_RZG2L_OTP_CHIPID_WORDS)

static uint64_t rzg2l_otp_read(void *opaque, hwaddr offset, unsigned size)
{
    RenesasRZG2LOTPState *s = opaque;

    if (offset >= OTP_CHIPID_OFFSET && offset < OTP_CHIPID_END) {
        return s->chip_id[(offset - OTP_CHIPID_OFFSET) / 4];
    }
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at offset 0x%"
                  HWADDR_PRIx "\n", __func__, offset);
    return 0;
}

static void rzg2l_otp_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at offset 0x%"
                  HWADDR_PRIx " value 0x%" PRIx64 "\n",
                  __func__, offset, value);
}

static const MemoryRegionOps rzg2l_otp_ops = {
    .read = rzg2l_otp_read,
    .write = rzg2l_otp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rzg2l_otp_init(Object *obj)
{
    RenesasRZG2LOTPState *s = RENESAS_RZG2L_OTP(obj);

    memory_region_init_io(&s->iomem, obj, &rzg2l_otp_ops, s,
                          TYPE_RENESAS_RZG2L_OTP,
                          RENESAS_RZG2L_OTP_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property rzg2l_otp_properties[] = {
    DEFINE_PROP_UINT32("chip-id0", RenesasRZG2LOTPState, chip_id[0], 0),
    DEFINE_PROP_UINT32("chip-id1", RenesasRZG2LOTPState, chip_id[1], 0),
    DEFINE_PROP_UINT32("chip-id2", RenesasRZG2LOTPState, chip_id[2], 0),
    DEFINE_PROP_UINT32("chip-id3", RenesasRZG2LOTPState, chip_id[3], 0),
};

static void rzg2l_otp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, rzg2l_otp_properties);
    dc->desc = "Renesas RZ/G2L family OTP (CHIP Product ID)";
}

static const TypeInfo rzg2l_otp_info = {
    .name = TYPE_RENESAS_RZG2L_OTP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZG2LOTPState),
    .instance_init = rzg2l_otp_init,
    .class_init = rzg2l_otp_class_init,
};

static void rzg2l_otp_register_types(void)
{
    type_register_static(&rzg2l_otp_info);
}

type_init(rzg2l_otp_register_types)
