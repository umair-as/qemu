/*
 * Renesas RZ/G2L family interrupt control unit (IA55)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The register file of the IA55 interrupt control unit of the Renesas
 * RZ/G2L family (RZ/G2L, RZ/V2L), which handles the NMI, IRQ0-7 and
 * TINT0-31 external interrupts: RZ/V2L Group User's Manual: Hardware
 * (R01UH0936EJ), sections 8.5 and 8.6.
 *
 * QEMU interface:
 *  + sysbus MMIO region 0: the IA55 registers
 *  + sysbus IRQs 0-7: the IRQ0-7 interrupts to the GIC, active high
 *  + named GPIO inputs "irq" 0-7: the level of pin IRQn
 */

#ifndef HW_INTC_RENESAS_RZG2L_IRQC_H
#define HW_INTC_RENESAS_RZG2L_IRQC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RZG2L_IRQC "renesas-rzg2l-irqc"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZG2LIRQCState, RENESAS_RZG2L_IRQC)

#define RENESAS_RZG2L_IRQC_MMIO_SIZE 0x10000
#define RENESAS_RZG2L_IRQC_NUM_REGS 19
#define RENESAS_RZG2L_IRQC_NUM_IRQ 8

struct RenesasRZG2LIRQCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[RENESAS_RZG2L_IRQC_NUM_REGS];
    bool irq_pin[RENESAS_RZG2L_IRQC_NUM_IRQ];
    qemu_irq irq_out[RENESAS_RZG2L_IRQC_NUM_IRQ];
};

#endif
