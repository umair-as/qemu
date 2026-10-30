/*
 * Renesas RZ/G2L family watchdog timer (WDT)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * One channel of the watchdog timer, RZ/V2L Group User's Manual:
 * Hardware, section 21: the nine registers of Table 21.4, all 32-bit.
 *
 * - Writing 1 to WDTCNT.WDTEN starts the 32-bit counter from WDTTIM.
 *   Nothing but a reset stops it (section 21.3.1). WDTSET and WDTTIM
 *   cannot be written while it runs; such writes are ignored and logged.
 * - The counter overflows when it passes WDTSET + H'000F_FFFF, that is
 *   every (WDTTIME + 1) x 1024 x 1024 clocks, and restarts from 0
 *   (sections 21.3.2, 21.4.1). The first overflow sets WDTINT.INTDISP
 *   and the WDTINT interrupt; a second overflow before INTDISP is
 *   cleared asserts WDTRSTB. Writing 1 to INTDISP clears the interrupt
 *   and the counter (section 21.3.4). A start value above the overflow
 *   value counts to the 32-bit limit and wraps to 0 first, without an
 *   interrupt (section 21.3.3; that the wrap raises no interrupt is an
 *   inference).
 * - PEEN = 1 forces a parity error: WDTRSTB when PECR is 0, PERROUT
 *   otherwise (Table 21.5, rows 7 and 8; the table gives PECR as one
 *   value, and the model reads it as "any bit set").
 * The counter runs on the virtual clock at WDTn_CLK and is computed when
 * WDTTIM is read; a QEMU timer fires only at the next overflow.
 *
 * Not modelled: CPU parity error inputs (PESR, PEER and PEPO are stored
 * only), the debug pause input, and the synchronisation delay between a
 * register write and its effect (section 21.4.3). While the clock
 * controller holds the channel in reset, its registers read their
 * reset values and writes are ignored. Offsets the manual does not
 * list, and accesses that are not 32-bit, fail with a decode error.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/host-utils.h"
#include "trace.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/watchdog/renesas_rzg2l_wdt.h"

REG32(WDTCNT, 0x00)
    FIELD(WDTCNT, WDTEN, 0, 1)
REG32(WDTSET, 0x04)
    FIELD(WDTSET, WDTTIME, 20, 12)
REG32(WDTTIM, 0x08)
REG32(WDTINT, 0x0c)
    FIELD(WDTINT, INTDISP, 0, 1)
REG32(PECR, 0x10)
REG32(PEEN, 0x14)
    FIELD(PEEN, PEEN, 0, 1)
REG32(PESR, 0x18)
REG32(PEER, 0x1c)
REG32(PEPO, 0x20)

#define WDTSET_RESET        0xfff00000u
/* The counter compares with WDTSET + H'000F_FFFF (section 21.3.2) */
#define WDT_COUNT_SPAN      0x00100000u
#define WDT_WRAP            (1ull << 32)

/* Clocks from the start of the count to the overflow */
static uint64_t wdt_period(RenesasRZG2LWDTState *s)
{
    return (uint64_t)s->wdtset + WDT_COUNT_SPAN;
}

static uint64_t wdt_ticks(RenesasRZG2LWDTState *s, int64_t now)
{
    return muldiv64(now - s->epoch_ns, s->freq_hz, NANOSECONDS_PER_SECOND);
}

/* The virtual time at which the counter has run @ticks clocks */
static int64_t wdt_time_of(RenesasRZG2LWDTState *s, uint64_t ticks)
{
    int64_t ns = muldiv64(ticks, NANOSECONDS_PER_SECOND, s->freq_hz);

    if (muldiv64(ns, s->freq_hz, NANOSECONDS_PER_SECOND) < ticks) {
        ns++;
    }
    return s->epoch_ns + ns;
}

/* Clocks from the epoch to the next overflow or wrap */
static uint64_t wdt_next_event(RenesasRZG2LWDTState *s)
{
    uint64_t period = wdt_period(s);

    if (s->count_at_epoch < period) {
        return period - s->count_at_epoch;
    }
    return WDT_WRAP - s->count_at_epoch;
}

static uint32_t wdt_count(RenesasRZG2LWDTState *s)
{
    if (!s->running) {
        return s->wdttim;
    }
    return s->count_at_epoch +
           wdt_ticks(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void wdt_update(RenesasRZG2LWDTState *s)
{
    bool forced = s->peen & R_PEEN_PEEN_MASK;

    qemu_set_irq(s->irq_wdtint, s->intdisp);
    qemu_set_irq(s->irq_perrout, forced && s->pecr);
    qemu_set_irq(s->wdtrstb, s->overflow_reset || (forced && !s->pecr));
}

/* Restart the count from @count at @now */
static void wdt_restart_count(RenesasRZG2LWDTState *s, int64_t now,
                              uint64_t count)
{
    s->epoch_ns = now;
    s->count_at_epoch = count;
    timer_mod(s->timer, wdt_time_of(s, wdt_next_event(s)));
}

static void wdt_timer_cb(void *opaque)
{
    RenesasRZG2LWDTState *s = RENESAS_RZG2L_WDT(opaque);
    uint64_t event = wdt_next_event(s);
    int64_t when = wdt_time_of(s, event);

    if (!s->running) {
        return;
    }
    if (wdt_ticks(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) < event) {
        timer_mod(s->timer, when);
        return;
    }
    if (s->count_at_epoch < wdt_period(s)) {
        if (!s->intdisp) {
            s->intdisp = true;
        } else {
            s->overflow_reset = true;
        }
        trace_renesas_rzg2l_wdt_overflow(s->intdisp, s->overflow_reset);
        wdt_update(s);
    }
    wdt_restart_count(s, when, 0);
}

static void wdt_reset_state(RenesasRZG2LWDTState *s)
{
    s->running = false;
    s->wdtset = WDTSET_RESET;
    s->wdttim = 0;
    s->intdisp = false;
    s->pecr = 0;
    s->peen = 0;
    s->pesr = 0;
    s->peer = 0;
    s->pepo = 0;
    s->epoch_ns = 0;
    s->count_at_epoch = 0;
    s->overflow_reset = false;
    if (s->timer) {
        timer_del(s->timer);
    }
}

static MemTxResult wdt_read(void *opaque, hwaddr offset, uint64_t *data,
                            unsigned size, MemTxAttrs attrs)
{
    RenesasRZG2LWDTState *s = RENESAS_RZG2L_WDT(opaque);
    uint64_t value;

    switch (offset) {
    case A_WDTCNT:
        value = s->running;
        break;
    case A_WDTSET:
        value = s->wdtset;
        break;
    case A_WDTTIM:
        value = wdt_count(s);
        break;
    case A_WDTINT:
        value = s->intdisp;
        break;
    case A_PECR:
        value = s->pecr;
        break;
    case A_PEEN:
        value = s->peen;
        break;
    case A_PESR:
        value = s->pesr;
        break;
    case A_PEER:
        value = s->peer;
        break;
    case A_PEPO:
        value = s->pepo;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "renesas_rzg2l_wdt: unimplemented read at "
                      "offset 0x%" HWADDR_PRIx "\n", offset);
        return MEMTX_DECODE_ERROR;
    }
    trace_renesas_rzg2l_wdt_read(offset, value);
    *data = value;
    return MEMTX_OK;
}

static MemTxResult wdt_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size, MemTxAttrs attrs)
{
    RenesasRZG2LWDTState *s = RENESAS_RZG2L_WDT(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (offset > A_PEPO) {
        qemu_log_mask(LOG_UNIMP, "renesas_rzg2l_wdt: unimplemented write at "
                      "offset 0x%" HWADDR_PRIx "\n", offset);
        return MEMTX_DECODE_ERROR;
    }
    trace_renesas_rzg2l_wdt_write(offset, value);
    if (s->in_reset) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_rzg2l_wdt: write at offset "
                      "0x%" HWADDR_PRIx " while held in reset is ignored\n",
                      offset);
        return MEMTX_OK;
    }
    switch (offset) {
    case A_WDTCNT:
        if (s->running) {
            if (!FIELD_EX32(value, WDTCNT, WDTEN)) {
                qemu_log_mask(LOG_GUEST_ERROR, "renesas_rzg2l_wdt: the "
                              "running watchdog cannot be stopped\n");
            }
            break;
        }
        if (FIELD_EX32(value, WDTCNT, WDTEN)) {
            s->running = true;
            wdt_restart_count(s, now, s->wdttim);
        }
        break;
    case A_WDTSET:
    case A_WDTTIM:
        if (s->running) {
            qemu_log_mask(LOG_GUEST_ERROR, "renesas_rzg2l_wdt: write at "
                          "offset 0x%" HWADDR_PRIx " while running is "
                          "ignored\n", offset);
        } else if (offset == A_WDTSET) {
            s->wdtset = value & R_WDTSET_WDTTIME_MASK;
        } else {
            s->wdttim = value;
        }
        break;
    case A_WDTINT:
        if (!FIELD_EX32(value, WDTINT, INTDISP)) {
            break;
        }
        s->intdisp = false;
        if (s->running) {
            wdt_restart_count(s, now, 0);
        } else {
            s->wdttim = 0;
        }
        break;
    case A_PECR:
        s->pecr = value;
        break;
    case A_PEEN:
        s->peen = value & R_PEEN_PEEN_MASK;
        break;
    case A_PESR:
        s->pesr = value;
        break;
    case A_PEER:
        s->peer = value;
        break;
    case A_PEPO:
        s->pepo = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "renesas_rzg2l_wdt: unimplemented write at "
                      "offset 0x%" HWADDR_PRIx "\n", offset);
        return MEMTX_DECODE_ERROR;
    }
    wdt_update(s);
    return MEMTX_OK;
}

static const MemoryRegionOps wdt_ops = {
    .read_with_attrs = wdt_read,
    .write_with_attrs = wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* PRESETN from the clock controller: @level 1 holds the channel in reset */
static void wdt_presetn(void *opaque, int n, int level)
{
    RenesasRZG2LWDTState *s = RENESAS_RZG2L_WDT(opaque);

    trace_renesas_rzg2l_wdt_presetn(level);
    s->in_reset = level;
    if (level) {
        wdt_reset_state(s);
        wdt_update(s);
    }
}

static void wdt_reset_hold(Object *obj, ResetType type)
{
    wdt_reset_state(RENESAS_RZG2L_WDT(obj));
}

static void wdt_reset_exit(Object *obj, ResetType type)
{
    wdt_update(RENESAS_RZG2L_WDT(obj));
}

static void wdt_init(Object *obj)
{
    RenesasRZG2LWDTState *s = RENESAS_RZG2L_WDT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &wdt_ops, s, TYPE_RENESAS_RZG2L_WDT,
                          RENESAS_RZG2L_WDT_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_wdtint);
    sysbus_init_irq(sbd, &s->irq_perrout);
    qdev_init_gpio_out_named(DEVICE(obj), &s->wdtrstb, "wdtrstb", 1);
    qdev_init_gpio_in_named(DEVICE(obj), wdt_presetn, "presetn", 1);
}

static void wdt_realize(DeviceState *dev, Error **errp)
{
    RenesasRZG2LWDTState *s = RENESAS_RZG2L_WDT(dev);

    if (!s->freq_hz) {
        error_setg(errp, "property 'clock-frequency' must be set");
        return;
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_timer_cb, s);
}

static void wdt_finalize(Object *obj)
{
    RenesasRZG2LWDTState *s = RENESAS_RZG2L_WDT(obj);

    if (s->timer) {
        timer_free(s->timer);
    }
}

static const VMStateDescription vmstate_wdt = {
    .name = TYPE_RENESAS_RZG2L_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(running, RenesasRZG2LWDTState),
        VMSTATE_UINT32(wdtset, RenesasRZG2LWDTState),
        VMSTATE_UINT32(wdttim, RenesasRZG2LWDTState),
        VMSTATE_BOOL(intdisp, RenesasRZG2LWDTState),
        VMSTATE_UINT32(pecr, RenesasRZG2LWDTState),
        VMSTATE_UINT32(peen, RenesasRZG2LWDTState),
        VMSTATE_UINT32(pesr, RenesasRZG2LWDTState),
        VMSTATE_UINT32(peer, RenesasRZG2LWDTState),
        VMSTATE_UINT32(pepo, RenesasRZG2LWDTState),
        VMSTATE_INT64(epoch_ns, RenesasRZG2LWDTState),
        VMSTATE_UINT64(count_at_epoch, RenesasRZG2LWDTState),
        VMSTATE_BOOL(overflow_reset, RenesasRZG2LWDTState),
        VMSTATE_BOOL(in_reset, RenesasRZG2LWDTState),
        VMSTATE_TIMER_PTR(timer, RenesasRZG2LWDTState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property wdt_props[] = {
    DEFINE_PROP_UINT32("clock-frequency", RenesasRZG2LWDTState, freq_hz, 0),
};

static void wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = wdt_realize;
    dc->vmsd = &vmstate_wdt;
    rc->phases.hold = wdt_reset_hold;
    rc->phases.exit = wdt_reset_exit;
    device_class_set_props(dc, wdt_props);
}

static const TypeInfo wdt_info = {
    .name = TYPE_RENESAS_RZG2L_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZG2LWDTState),
    .instance_init = wdt_init,
    .instance_finalize = wdt_finalize,
    .class_init = wdt_class_init,
};

static void wdt_register_types(void)
{
    type_register_static(&wdt_info);
}

type_init(wdt_register_types);
