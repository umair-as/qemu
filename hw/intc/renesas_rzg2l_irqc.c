/*
 * Renesas RZ/G2L family interrupt control unit (IA55)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The 19 registers of manual Table 8.3, all 32-bit, all resetting to 0.
 * The type and source selection registers hold what is written to
 * their implemented bits. The status registers (NSCR, ISCR, TSCR,
 * BEISR0/1, EREISR0/1) report detected interrupts; a bit can only be
 * cleared, by writing 0 to it while it is 1 (section 8.6).
 *
 * IRQ0-7: each input is the level of its IRQn pin, high when nothing
 * drives it. IITSR selects how it is detected (section 8.6.4): with
 * low-level detection ISCR.ISTATn follows the pin (1 while it is low)
 * and cannot be written; with an edge detection the edge sets ISTATn,
 * and a 0 written while it is 1 clears it (section 8.6.3, 8.7.2).
 * Each ISTATn drives the interrupt to GIC SPI 1 + n, active high.
 * The pin function selection in the PFC is not checked: the board
 * connects each input to the device that drives the pin.
 *
 * Not modelled: the NMI and TINT0-31 inputs and the interrupts they
 * send on to the GIC, the bus error and ECC RAM error inputs, and the
 * IRQ pin noise filter. With no input, nothing is detected, so those
 * status bits read 0, and NSCR.NSMON (the NMI pin level) reads 0.
 * Offsets the manual does not list, and accesses other than 32-bit,
 * fail with a decode error.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "hw/intc/renesas_rzg2l_irqc.h"

typedef struct {
    uint16_t offset;
    const char *name;
    bool status;        /* detection status: clear-only, reads 0 here */
    uint32_t rw;        /* writable bits of a selection register */
} IRQCReg;

static const IRQCReg irqc_regs[] = {
    { 0x00, "NSCR",    true,  0 },
    { 0x04, "NITSR",   false, 0x00000001 },
    { 0x10, "ISCR",    true,  0 },
    { 0x14, "IITSR",   false, 0x0000ffff },
    { 0x20, "TSCR",    true,  0 },
    { 0x24, "TITSR0",  false, 0xffffffff },
    { 0x28, "TITSR1",  false, 0xffffffff },
    { 0x30, "TSSR0",   false, 0xffffffff },
    { 0x34, "TSSR1",   false, 0xffffffff },
    { 0x38, "TSSR2",   false, 0xffffffff },
    { 0x3c, "TSSR3",   false, 0xffffffff },
    { 0x40, "TSSR4",   false, 0xffffffff },
    { 0x44, "TSSR5",   false, 0xffffffff },
    { 0x48, "TSSR6",   false, 0xffffffff },
    { 0x4c, "TSSR7",   false, 0xffffffff },
    { 0x50, "BEISR0",  true,  0 },
    { 0x54, "BEISR1",  true,  0 },
    { 0x60, "EREISR0", true,  0 },
    { 0x64, "EREISR1", true,  0 },
};

QEMU_BUILD_BUG_ON(ARRAY_SIZE(irqc_regs) != RENESAS_RZG2L_IRQC_NUM_REGS);

#define IRQC_ISCR   2
#define IRQC_IITSR  3

#define IITSEL_LOW_LEVEL 0
#define IITSEL_FALLING   1
#define IITSEL_RISING    2

static unsigned irqc_iitsel(RenesasRZG2LIRQCState *s, int n)
{
    return extract32(s->regs[IRQC_IITSR], 2 * n, 2);
}

/* Level detection follows the pins; then the lines follow ISCR */
static void irqc_update(RenesasRZG2LIRQCState *s)
{
    int n;

    for (n = 0; n < RENESAS_RZG2L_IRQC_NUM_IRQ; n++) {
        if (irqc_iitsel(s, n) == IITSEL_LOW_LEVEL) {
            s->regs[IRQC_ISCR] = deposit32(s->regs[IRQC_ISCR], n, 1,
                                           !s->irq_pin[n]);
        }
        qemu_set_irq(s->irq_out[n], extract32(s->regs[IRQC_ISCR], n, 1));
    }
}

static void irqc_set_irq_pin(void *opaque, int n, int level)
{
    RenesasRZG2LIRQCState *s = RENESAS_RZG2L_IRQC(opaque);
    bool old = s->irq_pin[n];
    unsigned type = irqc_iitsel(s, n);

    s->irq_pin[n] = level;
    trace_renesas_rzg2l_irqc_irq_pin(n, level);
    if (type != IITSEL_LOW_LEVEL && old != s->irq_pin[n]) {
        /* 01 falling, 10 rising, 11 both */
        if ((s->irq_pin[n] && (type & IITSEL_RISING)) ||
            (!s->irq_pin[n] && (type & IITSEL_FALLING))) {
            s->regs[IRQC_ISCR] |= BIT(n);
        }
    }
    irqc_update(s);
}

static int irqc_lookup(hwaddr offset)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(irqc_regs); i++) {
        if (irqc_regs[i].offset == offset) {
            return i;
        }
    }
    return -1;
}

static MemTxResult irqc_read(void *opaque, hwaddr offset, uint64_t *data,
                             unsigned size, MemTxAttrs attrs)
{
    RenesasRZG2LIRQCState *s = RENESAS_RZG2L_IRQC(opaque);
    int i = irqc_lookup(offset);

    if (i < 0) {
        qemu_log_mask(LOG_UNIMP, "renesas_rzg2l_irqc: unimplemented read "
                      "at offset 0x%" HWADDR_PRIx "\n", offset);
        return MEMTX_DECODE_ERROR;
    }
    *data = s->regs[i];
    trace_renesas_rzg2l_irqc_read(irqc_regs[i].name, offset, *data);
    return MEMTX_OK;
}

static MemTxResult irqc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    RenesasRZG2LIRQCState *s = RENESAS_RZG2L_IRQC(opaque);
    int i = irqc_lookup(offset);

    if (i < 0) {
        qemu_log_mask(LOG_UNIMP, "renesas_rzg2l_irqc: unimplemented write "
                      "at offset 0x%" HWADDR_PRIx "\n", offset);
        return MEMTX_DECODE_ERROR;
    }
    trace_renesas_rzg2l_irqc_write(irqc_regs[i].name, offset, value);
    if (i == IRQC_ISCR) {
        int n;

        /* Only an edge-detected status clears, by a 0 */
        for (n = 0; n < RENESAS_RZG2L_IRQC_NUM_IRQ; n++) {
            if (irqc_iitsel(s, n) != IITSEL_LOW_LEVEL && !(value & BIT(n))) {
                s->regs[i] &= ~BIT(n);
            }
        }
        irqc_update(s);
        return MEMTX_OK;
    }
    if (irqc_regs[i].status) {
        /* A 0 clears a set bit, a 1 is "invalid to write" */
        s->regs[i] &= value;
        return MEMTX_OK;
    }
    s->regs[i] = (s->regs[i] & ~irqc_regs[i].rw) | (value & irqc_regs[i].rw);
    if (i == IRQC_IITSR) {
        irqc_update(s);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps irqc_ops = {
    .read_with_attrs = irqc_read,
    .write_with_attrs = irqc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void irqc_reset_hold(Object *obj, ResetType type)
{
    RenesasRZG2LIRQCState *s = RENESAS_RZG2L_IRQC(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void irqc_reset_exit(Object *obj, ResetType type)
{
    irqc_update(RENESAS_RZG2L_IRQC(obj));
}

static void irqc_init(Object *obj)
{
    RenesasRZG2LIRQCState *s = RENESAS_RZG2L_IRQC(obj);
    int n;

    memory_region_init_io(&s->iomem, obj, &irqc_ops, s,
                          TYPE_RENESAS_RZG2L_IRQC,
                          RENESAS_RZG2L_IRQC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (n = 0; n < RENESAS_RZG2L_IRQC_NUM_IRQ; n++) {
        s->irq_pin[n] = true;
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_out[n]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), irqc_set_irq_pin, "irq",
                            RENESAS_RZG2L_IRQC_NUM_IRQ);
}

static const VMStateDescription vmstate_irqc = {
    .name = TYPE_RENESAS_RZG2L_IRQC,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RenesasRZG2LIRQCState,
                             RENESAS_RZG2L_IRQC_NUM_REGS),
        VMSTATE_BOOL_ARRAY(irq_pin, RenesasRZG2LIRQCState,
                           RENESAS_RZG2L_IRQC_NUM_IRQ),
        VMSTATE_END_OF_LIST()
    }
};

static void irqc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_irqc;
    rc->phases.hold = irqc_reset_hold;
    rc->phases.exit = irqc_reset_exit;
}

static const TypeInfo irqc_info = {
    .name = TYPE_RENESAS_RZG2L_IRQC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZG2LIRQCState),
    .instance_init = irqc_init,
    .class_init = irqc_class_init,
};

static void irqc_register_types(void)
{
    type_register_static(&irqc_info);
}

type_init(irqc_register_types);
