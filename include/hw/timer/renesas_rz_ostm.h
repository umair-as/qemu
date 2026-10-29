/*
 * Renesas RZ OS timer (OSTM)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * One channel of the OS timer of the Renesas RZ/G2L family (RZ/G2L,
 * RZ/V2L), which the RZ/V2L Group User's Manual: Hardware (R01UH0936EJ)
 * documents as the General Timer (GTM), section 20.
 *
 * QEMU interface:
 *  + QOM property "clock-frequency": the count clock (P0) in Hz; must be
 *    set
 *  + sysbus MMIO region 0: the channel's registers
 *  + sysbus IRQ 0: OSTMnTINT, pulsed once per interrupt request (held
 *    high in interval mode with a compare value of 0)
 */

#ifndef HW_TIMER_RENESAS_RZ_OSTM_H
#define HW_TIMER_RENESAS_RZ_OSTM_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_RENESAS_RZ_OSTM "renesas-rz-ostm"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZOSTMState, RENESAS_RZ_OSTM)

#define RENESAS_RZ_OSTM_MMIO_SIZE 0x400

struct RenesasRZOSTMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;

    uint32_t cmp;
    uint8_t ctl;
    bool te;
    /* Counter value while stopped */
    uint32_t cnt;
    /* While running: start of the current count, in virtual-clock ns */
    int64_t epoch_ns;
    /* Interval mode: the start value latched at the last (re)load */
    uint32_t reload;
    /* Free-running mode: clocks from epoch_ns to the next compare match */
    uint64_t next_match;
    bool level;

    /* Properties */
    uint32_t freq_hz;
};

#endif
