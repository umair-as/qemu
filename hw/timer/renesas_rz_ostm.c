/*
 * Renesas RZ OS timer (OSTM)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * One channel of the OS timer, documented as the General Timer (GTM) in
 * the RZ/V2L Group User's Manual: Hardware, section 20.
 *
 * The counter runs on the virtual clock at the count clock frequency and
 * is computed when it is read. A QEMU timer fires only at the next
 * interrupt request:
 * - free-running comparison mode (CTL.MD1 = 1) counts up from 0 and
 *   requests an interrupt when the count passes CMP, the first one
 *   (CMP + 1) count clocks after the start (Table 20.5);
 * - interval timer mode (CTL.MD1 = 0) counts down from CMP and requests
 *   an interrupt every (CMP + 1) count clocks, reloading CMP each time
 *   (section 20.3.5.1). With CMP = 0 the request stays asserted while
 *   the counter runs (section 20.3.5.2).
 * A request is a pulse on the IRQ line, which the SoC wires to an
 * edge-triggered interrupt.
 *
 * Not modelled: DMA start by compare match, the debug pause input, and
 * the count clock following the clock controller's divider and gate.
 * Offsets the manual does not list, and accesses wider or narrower than
 * a register's access size, fail with a decode error.
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
#include "hw/timer/renesas_rz_ostm.h"

REG32(CMP, 0x00)
REG32(CNT, 0x04)
REG8(TE, 0x10)
    FIELD(TE, TE, 0, 1)
REG8(TS, 0x14)
    FIELD(TS, TS, 0, 1)
REG8(TT, 0x18)
    FIELD(TT, TT, 0, 1)
REG8(CTL, 0x20)
    FIELD(CTL, MD0, 0, 1)
    FIELD(CTL, MD1, 1, 1)

#define CTL_MASK            (R_CTL_MD0_MASK | R_CTL_MD1_MASK)

/* Counter value after a change to each operating mode (Table 20.4) */
#define CNT_INIT_INTERVAL   0xffffffffu
#define CNT_INIT_FREERUN    0x00000000u

#define WRAP                (1ull << 32)

static bool ostm_freerun(RenesasRZOSTMState *s)
{
    return FIELD_EX8(s->ctl, CTL, MD1);
}

static uint64_t ostm_ticks(RenesasRZOSTMState *s, int64_t now)
{
    return muldiv64(now - s->epoch_ns, s->freq_hz, NANOSECONDS_PER_SECOND);
}

/* The virtual time at which the counter has run @ticks count clocks */
static int64_t ostm_time_of(RenesasRZOSTMState *s, uint64_t ticks)
{
    int64_t ns = muldiv64(ticks, NANOSECONDS_PER_SECOND, s->freq_hz);

    if (muldiv64(ns, s->freq_hz, NANOSECONDS_PER_SECOND) < ticks) {
        ns++;
    }
    return s->epoch_ns + ns;
}

static void ostm_set_level(RenesasRZOSTMState *s, bool level)
{
    s->level = level;
    qemu_set_irq(s->irq, level);
}

/* One interrupt request (section 20.3.3) */
static void ostm_request(RenesasRZOSTMState *s)
{
    if (!s->level) {
        qemu_irq_pulse(s->irq);
    }
}

static uint32_t ostm_count(RenesasRZOSTMState *s)
{
    uint64_t t;

    if (!s->te) {
        return s->cnt;
    }
    t = ostm_ticks(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (ostm_freerun(s)) {
        return (uint32_t)t;
    }
    if (s->reload == 0) {
        return 0;
    }
    return s->reload - t % ((uint64_t)s->reload + 1);
}

/* Interval mode: count down from CMP, the next request after CMP + 1 */
static void ostm_interval_load(RenesasRZOSTMState *s, int64_t epoch)
{
    s->epoch_ns = epoch;
    s->reload = s->cmp;
    if (s->reload == 0) {
        timer_del(s->timer);
        ostm_set_level(s, true);
        return;
    }
    ostm_set_level(s, false);
    timer_mod(s->timer, ostm_time_of(s, (uint64_t)s->reload + 1));
}

/* Free-running mode: the first count after now at which CNT passes CMP */
static void ostm_freerun_arm(RenesasRZOSTMState *s, uint64_t now_ticks)
{
    uint64_t m = (uint64_t)s->cmp + 1;

    if (m <= now_ticks) {
        m += (now_ticks - m) / WRAP * WRAP + WRAP;
    }
    s->next_match = m;
    timer_mod(s->timer, ostm_time_of(s, m));
}

static void ostm_start(RenesasRZOSTMState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->te = true;
    if (FIELD_EX8(s->ctl, CTL, MD0)) {
        ostm_request(s);
    }
    if (ostm_freerun(s)) {
        s->epoch_ns = now;
        ostm_set_level(s, false);
        ostm_freerun_arm(s, 0);
    } else {
        ostm_interval_load(s, now);
    }
}

static void ostm_stop(RenesasRZOSTMState *s)
{
    s->cnt = ostm_count(s);
    s->te = false;
    timer_del(s->timer);
    ostm_set_level(s, false);
}

static void ostm_timer_cb(void *opaque)
{
    RenesasRZOSTMState *s = RENESAS_RZ_OSTM(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t t = ostm_ticks(s, now);

    if (!s->te) {
        return;
    }
    if (ostm_freerun(s)) {
        if (t < s->next_match) {
            timer_mod(s->timer, ostm_time_of(s, s->next_match));
            return;
        }
        ostm_request(s);
        ostm_freerun_arm(s, t);
        return;
    }

    if (t < (uint64_t)s->reload + 1) {
        timer_mod(s->timer, ostm_time_of(s, (uint64_t)s->reload + 1));
        return;
    }
    ostm_request(s);
    /* The counter reloads CMP: a new value written since takes effect */
    ostm_interval_load(s, ostm_time_of(s, (uint64_t)s->reload + 1));
}

static void ostm_write_cmp(RenesasRZOSTMState *s, uint32_t value)
{
    s->cmp = value;
    if (!s->te) {
        return;
    }
    if (ostm_freerun(s)) {
        /* Takes effect at once (Table 20.5) */
        ostm_freerun_arm(s, ostm_ticks(s, qemu_clock_get_ns(
                                              QEMU_CLOCK_VIRTUAL)));
    } else if (s->reload == 0) {
        /* The counter reloads CMP on every count clock while CMP is 0 */
        ostm_interval_load(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static unsigned ostm_reg_size(hwaddr offset)
{
    switch (offset) {
    case A_CMP:
    case A_CNT:
        return 4;
    case A_TE:
    case A_TS:
    case A_TT:
    case A_CTL:
        return 1;
    default:
        return 0;
    }
}

static MemTxResult ostm_check_access(hwaddr offset, unsigned size,
                                     const char *what)
{
    unsigned want = ostm_reg_size(offset);

    if (!want) {
        qemu_log_mask(LOG_UNIMP, "renesas_rz_ostm: unimplemented %s at "
                      "offset 0x%" HWADDR_PRIx "\n", what, offset);
        return MEMTX_DECODE_ERROR;
    }
    if (size != want) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_rz_ostm: %u-byte %s at "
                      "offset 0x%" HWADDR_PRIx " (access size %u)\n",
                      size, what, offset, want);
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult ostm_read(void *opaque, hwaddr offset, uint64_t *data,
                             unsigned size, MemTxAttrs attrs)
{
    RenesasRZOSTMState *s = RENESAS_RZ_OSTM(opaque);
    MemTxResult r = ostm_check_access(offset, size, "read");
    uint64_t value = 0;

    if (r != MEMTX_OK) {
        return r;
    }
    switch (offset) {
    case A_CMP:
        value = s->cmp;
        break;
    case A_CNT:
        value = ostm_count(s);
        break;
    case A_TE:
        value = s->te;
        break;
    case A_CTL:
        value = s->ctl;
        break;
    default:
        /* TS and TT always read as 0 */
        break;
    }
    trace_renesas_rz_ostm_read(offset, value, size);
    *data = value;
    return MEMTX_OK;
}

static MemTxResult ostm_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    RenesasRZOSTMState *s = RENESAS_RZ_OSTM(opaque);
    MemTxResult r = ostm_check_access(offset, size, "write");

    if (r != MEMTX_OK) {
        return r;
    }
    trace_renesas_rz_ostm_write(offset, value, size);
    switch (offset) {
    case A_CMP:
        ostm_write_cmp(s, value);
        break;
    case A_TS:
        if (!FIELD_EX8(value, TS, TS)) {
            break;
        }
        /* A restart while running: forced in interval mode only */
        if (!s->te || !ostm_freerun(s)) {
            ostm_start(s);
        }
        break;
    case A_TT:
        if (FIELD_EX8(value, TT, TT) && s->te) {
            ostm_stop(s);
        }
        break;
    case A_CTL:
        if (s->te) {
            qemu_log_mask(LOG_GUEST_ERROR, "renesas_rz_ostm: CTL write "
                          "while the counter runs is ignored\n");
            break;
        }
        if (FIELD_EX8(value, CTL, MD1) != FIELD_EX8(s->ctl, CTL, MD1)) {
            s->cnt = FIELD_EX8(value, CTL, MD1) ? CNT_INIT_FREERUN
                                                : CNT_INIT_INTERVAL;
        }
        s->ctl = value & CTL_MASK;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_rz_ostm: write to "
                      "read-only offset 0x%" HWADDR_PRIx "\n", offset);
        break;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ostm_ops = {
    .read_with_attrs = ostm_read,
    .write_with_attrs = ostm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void ostm_reset_hold(Object *obj, ResetType type)
{
    RenesasRZOSTMState *s = RENESAS_RZ_OSTM(obj);

    /* Section 20.2.1: CNT resets to the interval-mode value */
    s->cmp = 0;
    s->ctl = 0;
    s->te = false;
    s->cnt = CNT_INIT_INTERVAL;
    s->epoch_ns = 0;
    s->reload = 0;
    s->next_match = 0;
    if (s->timer) {
        timer_del(s->timer);
    }
    ostm_set_level(s, false);
}

static void ostm_init(Object *obj)
{
    RenesasRZOSTMState *s = RENESAS_RZ_OSTM(obj);

    memory_region_init_io(&s->iomem, obj, &ostm_ops, s, TYPE_RENESAS_RZ_OSTM,
                          RENESAS_RZ_OSTM_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void ostm_realize(DeviceState *dev, Error **errp)
{
    RenesasRZOSTMState *s = RENESAS_RZ_OSTM(dev);

    if (!s->freq_hz) {
        error_setg(errp, "property 'clock-frequency' must be set");
        return;
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ostm_timer_cb, s);
}

static void ostm_finalize(Object *obj)
{
    RenesasRZOSTMState *s = RENESAS_RZ_OSTM(obj);

    if (s->timer) {
        timer_free(s->timer);
    }
}

static const VMStateDescription vmstate_ostm = {
    .name = TYPE_RENESAS_RZ_OSTM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmp, RenesasRZOSTMState),
        VMSTATE_UINT8(ctl, RenesasRZOSTMState),
        VMSTATE_BOOL(te, RenesasRZOSTMState),
        VMSTATE_UINT32(cnt, RenesasRZOSTMState),
        VMSTATE_INT64(epoch_ns, RenesasRZOSTMState),
        VMSTATE_UINT32(reload, RenesasRZOSTMState),
        VMSTATE_UINT64(next_match, RenesasRZOSTMState),
        VMSTATE_BOOL(level, RenesasRZOSTMState),
        VMSTATE_TIMER_PTR(timer, RenesasRZOSTMState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ostm_props[] = {
    DEFINE_PROP_UINT32("clock-frequency", RenesasRZOSTMState, freq_hz, 0),
};

static void ostm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = ostm_realize;
    dc->vmsd = &vmstate_ostm;
    rc->phases.hold = ostm_reset_hold;
    device_class_set_props(dc, ostm_props);
}

static const TypeInfo ostm_info = {
    .name = TYPE_RENESAS_RZ_OSTM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZOSTMState),
    .instance_init = ostm_init,
    .instance_finalize = ostm_finalize,
    .class_init = ostm_class_init,
};

static void ostm_register_types(void)
{
    type_register_static(&ostm_info);
}

type_init(ostm_register_types);
