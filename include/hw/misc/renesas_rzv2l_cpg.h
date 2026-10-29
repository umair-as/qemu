/*
 * Renesas RZ/V2L Clock Pulse Generator (CPG)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Model of the clock, reset and module-stop registers of the Clock
 * Pulse Generator of the Renesas RZ/V2L (R9A07G054), documented in the
 * RZ/V2L Group User's Manual: Hardware (R01UH0936EJ), section 7.
 *
 * QEMU interface:
 *  + QOM property "direct-boot": start from the register values the
 *    boot firmware leaves behind, instead of the reset values, for a
 *    kernel that is loaded directly
 *  + sysbus MMIO region 0: the CPG register bank
 */

#ifndef HW_MISC_RENESAS_RZV2L_CPG_H
#define HW_MISC_RENESAS_RZV2L_CPG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RZV2L_CPG "renesas-rzv2l-cpg"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZV2LCPGState, RENESAS_RZV2L_CPG)

#define RENESAS_RZV2L_CPG_MMIO_SIZE 0x10000

/* Number of registers in the model's register table */
#define RENESAS_RZV2L_CPG_NUM_REGS 189

struct RenesasRZV2LCPGState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    /* Indexed like the register table; monitors are computed, not kept */
    uint32_t regs[RENESAS_RZV2L_CPG_NUM_REGS];

    /* Properties */
    bool direct_boot;
};

#endif
