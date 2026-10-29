/*
 * Renesas RZ/G2L family pin function controller (PFC/GPIO)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The port and pin function registers of the Renesas RZ/G2L family
 * (RZ/G2L, RZ/V2L): RZ/V2L Group User's Manual: Hardware (R01UH0936EJ),
 * section 41.
 *
 * QEMU interface:
 *  + QOM property "direct-boot": start from the pin settings the boot
 *    firmware leaves behind, for a kernel that is loaded directly
 *  + sysbus MMIO region 0: the PFC registers
 */

#ifndef HW_GPIO_RENESAS_RZG2L_PFC_H
#define HW_GPIO_RENESAS_RZG2L_PFC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RZG2L_PFC "renesas-rzg2l-pfc"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZG2LPFCState, RENESAS_RZG2L_PFC)

#define RENESAS_RZG2L_PFC_MMIO_SIZE 0x10000
#define RENESAS_RZG2L_PFC_NUM_REGS 592

struct RenesasRZG2LPFCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[RENESAS_RZG2L_PFC_NUM_REGS];

    /* Properties */
    bool direct_boot;
};

#endif
