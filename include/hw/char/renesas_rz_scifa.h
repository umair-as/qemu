/*
 * Renesas RZ family Serial Communication Interface with FIFO (SCIFA)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Model of one SCIFA channel of the Renesas RZ/G2L family (RZ/G2L,
 * RZ/V2L), documented in the RZ/V2L Group User's Manual: Hardware
 * (R01UH0936EJ), section 22.
 *
 * QEMU interface:
 *  + QOM property "direct-boot": start in the state the boot firmware
 *    leaves the console channel in (see renesas_rz_scifa.c), instead of
 *    the reset values. Default off. Set by the board when it loads a
 *    kernel directly, without running the firmware.
 *  + sysbus MMIO region 0: the channel's register bank
 */

#ifndef HW_CHAR_RENESAS_RZ_SCIFA_H
#define HW_CHAR_RENESAS_RZ_SCIFA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RZ_SCIFA "renesas-rz-scifa"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZSCIFAState, RENESAS_RZ_SCIFA)

#define RENESAS_RZ_SCIFA_MMIO_SIZE 0x400

struct RenesasRZSCIFAState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    /* Registers */
    uint16_t smr;
    uint8_t brr;
    uint8_t mddr;
    uint16_t scr;
    uint16_t fsr;
    uint16_t fcr;
    uint16_t sptr;
    uint16_t lsr;
    uint8_t semr;
    uint16_t ftcr;

    /* Properties */
    bool direct_boot;
};

#endif
