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
 *  + named GPIO outputs "wdt-presetn" 0-2: high while CPG_RST_WDT holds
 *    watchdog channel n in reset
 *  + named GPIO outputs "sdhi-rst" 0-1: high while CPG_RST_SDHI holds
 *    SDHI channel n in reset (its IXRST line is low)
 *  + named GPIO inputs "wdtrstb" 0-2: the reset request of watchdog
 *    channel n, active high
 */

#ifndef HW_MISC_RENESAS_RZV2L_CPG_H
#define HW_MISC_RENESAS_RZV2L_CPG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RZV2L_CPG "renesas-rzv2l-cpg"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZV2LCPGState, RENESAS_RZV2L_CPG)

#define RENESAS_RZV2L_CPG_MMIO_SIZE 0x10000

/* Number of registers in the model's register table */
#define RENESAS_RZV2L_CPG_NUM_REGS 191

#define RENESAS_RZV2L_CPG_NUM_WDT 3
#define RENESAS_RZV2L_CPG_NUM_SDHI 2

struct RenesasRZV2LCPGState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    /* Indexed like the register table; monitors are computed, not kept */
    uint32_t regs[RENESAS_RZV2L_CPG_NUM_REGS];
    /* CPG_WDTOVF_RST: not cleared by the system reset it reports */
    uint32_t wdtovf;
    bool wdtrstb[RENESAS_RZV2L_CPG_NUM_WDT];
    qemu_irq wdt_presetn[RENESAS_RZV2L_CPG_NUM_WDT];
    qemu_irq sdhi_rst[RENESAS_RZV2L_CPG_NUM_SDHI];

    /* Properties */
    bool direct_boot;
};

#endif
