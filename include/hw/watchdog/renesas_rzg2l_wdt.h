/*
 * Renesas RZ/G2L family watchdog timer (WDT)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * One channel of the watchdog timer of the Renesas RZ/G2L family
 * (RZ/G2L, RZ/V2L), documented in the RZ/V2L Group User's Manual:
 * Hardware (R01UH0936EJ), section 21.
 *
 * QEMU interface:
 *  + QOM property "clock-frequency": the counter clock (WDTn_CLK) in Hz;
 *    must be set
 *  + sysbus MMIO region 0: the channel's registers
 *  + sysbus IRQ 0: WDTINT, high while an interrupt request is pending
 *  + sysbus IRQ 1: PERROUT, the parity error interrupt
 *  + named GPIO output "wdtrstb": high while the channel requests a
 *    reset (the active-low WDTRSTB signal of the manual is asserted);
 *    the clock controller decides whether it resets the system
 *  + named GPIO input "presetn": high while the clock controller holds
 *    the channel in reset (its PRESETN line is low)
 */

#ifndef HW_WATCHDOG_RENESAS_RZG2L_WDT_H
#define HW_WATCHDOG_RENESAS_RZG2L_WDT_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_RENESAS_RZG2L_WDT "renesas-rzg2l-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZG2LWDTState, RENESAS_RZG2L_WDT)

#define RENESAS_RZG2L_WDT_MMIO_SIZE 0x400

struct RenesasRZG2LWDTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq_wdtint;
    qemu_irq irq_perrout;
    qemu_irq wdtrstb;
    QEMUTimer *timer;

    bool running;
    uint32_t wdtset;
    /* Counter value while stopped (the WDTTIM register) */
    uint32_t wdttim;
    bool intdisp;
    uint32_t pecr;
    uint32_t peen;
    uint32_t pesr;
    uint32_t peer;
    uint32_t pepo;
    /* While running: the counter was count_at_epoch at epoch_ns */
    int64_t epoch_ns;
    uint64_t count_at_epoch;
    /* The second overflow came without a clear */
    bool overflow_reset;
    /* Held in reset by the clock controller */
    bool in_reset;

    /* Properties */
    uint32_t freq_hz;
};

#endif
