/*
 * Renesas RZ/G2L family One-Time Programmable memory (OTP)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Model of the direct-read view of the CHIP Product ID in the OTP of the
 * Renesas RZ/G2L family (RZ/G2L, RZ/V2L): RZ/G2L, RZ/G2LC, RZ/G2UL and
 * RZ/V2L Security Package, Security User's Manual (R11AN1002EJ), section
 * "OTP Memory Map", CHIP Product ID (OTPCPID). The ID is unique to each
 * device; the model's value is the "chip-id" properties, not a silicon
 * value. The rest of the OTP (its programming interface and the other
 * areas) is not modelled: accesses are logged as unimplemented, reads
 * return 0 and writes are ignored.
 *
 * QEMU interface:
 *  + QOM properties "chip-id0".."chip-id3": CHIPID[31:0] .. [127:96]
 *  + sysbus MMIO region 0: the 64 KiB OTP window
 */

#ifndef HW_MISC_RENESAS_RZG2L_OTP_H
#define HW_MISC_RENESAS_RZG2L_OTP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RZG2L_OTP "renesas-rzg2l-otp"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZG2LOTPState, RENESAS_RZG2L_OTP)

#define RENESAS_RZG2L_OTP_MMIO_SIZE 0x10000
#define RENESAS_RZG2L_OTP_CHIPID_WORDS 4

struct RenesasRZG2LOTPState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t chip_id[RENESAS_RZG2L_OTP_CHIPID_WORDS];
};

#endif
