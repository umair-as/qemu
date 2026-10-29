/*
 * Renesas RZ family Serial Communication Interface with FIFO (SCIFA)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * One SCIFA channel in asynchronous mode (manual section 22).
 *
 * Modelled: the register file with its reset values (Table 22.4), the
 * BRR/MDDR pair that shares offset 0x02 and is selected by SEMR.MDDRS,
 * and the rule that MDDR can only be written while SCR.TE = SCR.RE = 0
 * (section 22.2.9). SPTR bits 6, 4, 2 and 0 read the input level of
 * their pins (section 22.2.12); the pins are not modelled, so RXD reads
 * as an idle line (1) and the others as 0.
 *
 * FSR flags are cleared by writing 0 only after they have been read as
 * 1 (section 22.2.7). TDFE is set while the transmit FIFO holds no more
 * bytes than the transmit trigger, or while TE is 0, also right after a
 * clear (section 22.8.1). An FTDR write clears TEND and TDFE.
 *
 * Transmission takes time: a byte moves from the 16-byte transmit FIFO
 * into the shift register, goes to the chardev, and the next byte
 * follows one frame time later, on the virtual clock. TEND is set when
 * the last frame is out and the FIFO is empty; with TE cleared while
 * bytes are still queued it stays 0 (section 22.2.7). Firmware that
 * clears TEND right after an FTDR write and then waits for it relies on
 * this. The frame time follows the programmed format and bit rate
 * (section 22.2.8 and 22.2.9 formulas, with a 100 MHz P0 clock); the
 * bit rate is not otherwise enforced.
 *
 * Reception: bytes from the chardev enter the 16-byte receive FIFO. RDF
 * is set when the FIFO reaches the receive trigger, cleared by an FRDR
 * read or a read-armed 0-write, and set again while the FIFO holds more
 * than the trigger (section 22.8.2). DR is set 15 bit times after the
 * last byte while fewer bytes than the trigger wait (section 22.2.7).
 * From the chardev the FIFO cannot overflow: the host is held off when
 * it is full. In loopback (FCR.LOOP) the transmitter feeds the receiver
 * with no such flow control; a byte that finds the FIFO full is lost and
 * sets LSR.ORER, and the receiver then drops everything until ORER is
 * cleared (section 22.2.13). Framing, parity and break are never seen.
 *
 * The five interrupt lines are levels computed from the enable bits in
 * SCR and the flags (Table 22.19).
 *
 * Each register accepts only its documented access width. Any other
 * access to a register's bytes is answered with a decode error. Offsets
 * without a register read as 0 and ignore writes, as observed on the
 * RZ/V2L for a 16-bit read at 0x18.
 *
 * Clock synchronous mode is not modelled: an SMR write that sets CM is
 * refused with a decode error.
 *
 * The "direct-boot" property starts the channel in the state the
 * Renesas boot firmware (TF-A) leaves the console in: 115200 8N1 with
 * the transmitter and receiver enabled, as read back on an RZ/V2L
 * board at the U-Boot prompt.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/renesas_rz_scifa.h"
#include "migration/vmstate.h"

REG16(SMR, 0x00)
    FIELD(SMR, CKS, 0, 2)
    FIELD(SMR, STOP, 3, 1)
    FIELD(SMR, PM, 4, 1)
    FIELD(SMR, PE, 5, 1)
    FIELD(SMR, CHR, 6, 1)
    FIELD(SMR, CM, 7, 1)
REG8(BRR, 0x02)               /* MDDR at the same offset (SEMR.MDDRS) */
REG16(SCR, 0x04)
    FIELD(SCR, CKE, 0, 2)
    FIELD(SCR, TEIE, 2, 1)
    FIELD(SCR, REIE, 3, 1)
    FIELD(SCR, RE, 4, 1)
    FIELD(SCR, TE, 5, 1)
    FIELD(SCR, RIE, 6, 1)
    FIELD(SCR, TIE, 7, 1)
REG8(FTDR, 0x06)
REG16(FSR, 0x08)
    FIELD(FSR, DR, 0, 1)
    FIELD(FSR, RDF, 1, 1)
    FIELD(FSR, PER, 2, 1)
    FIELD(FSR, FER, 3, 1)
    FIELD(FSR, BRK, 4, 1)
    FIELD(FSR, TDFE, 5, 1)
    FIELD(FSR, TEND, 6, 1)
    FIELD(FSR, ER, 7, 1)
REG8(FRDR, 0x0a)
REG16(FCR, 0x0c)
    FIELD(FCR, LOOP, 0, 1)
    FIELD(FCR, RFRST, 1, 1)
    FIELD(FCR, TFRST, 2, 1)
    FIELD(FCR, MCE, 3, 1)
    FIELD(FCR, TTRG, 4, 2)
    FIELD(FCR, RTRG, 6, 2)
    FIELD(FCR, RSTRG, 8, 3)
REG16(FDR, 0x0e)
    FIELD(FDR, R, 0, 5)
    FIELD(FDR, T, 8, 5)
REG16(SPTR, 0x10)
    FIELD(SPTR, SPB2DT, 0, 1)
REG16(LSR, 0x12)
    FIELD(LSR, ORER, 0, 1)
REG8(SEMR, 0x14)
    FIELD(SEMR, ABCS0, 0, 1)
    FIELD(SEMR, NFEN, 2, 1)
    FIELD(SEMR, DIR, 3, 1)
    FIELD(SEMR, MDDRS, 4, 1)
    FIELD(SEMR, BRME, 5, 1)
    FIELD(SEMR, BGDM, 7, 1)
REG16(FTCR, 0x16)
    FIELD(FTCR, TFTC, 0, 5)
    FIELD(FTCR, TTRGS, 7, 1)
    FIELD(FTCR, RFTC, 8, 5)
    FIELD(FTCR, RTRGS, 15, 1)

/* Writable bits; the others are reserved and read as 0 */
#define SMR_WMASK   0x00fb
#define SCR_WMASK   0x00ff
#define FCR_WMASK   0x07ff
#define SPTR_WMASK  0x00ff
#define SEMR_WMASK  0xbd
#define FTCR_WMASK  0x9f9f

/* FSR flags cleared by a 0-write after a 1-read; FER and PER are R */
#define FSR_CLEARABLE   (R_FSR_ER_MASK | R_FSR_TEND_MASK | \
                         R_FSR_TDFE_MASK | R_FSR_BRK_MASK | \
                         R_FSR_RDF_MASK | R_FSR_DR_MASK)

/* SPTR bits that read the pin input level instead of the written value */
#define SPTR_PIN_BITS 0x0055

/* MDDR bit 7 is fixed to 1 (section 22.2.9) */
#define MDDR_FIXED  0x80

/* Peripheral clock P0 of the RZ/V2L, which drives the baud generator */
#define RZ_SCIFA_P0_HZ  100000000ULL

/* Reset values, Table 22.4 */
#define SMR_RESET   0x0000
#define BRR_RESET   0xff
#define MDDR_RESET  0xff
#define SCR_RESET   0x0000
#define FSR_RESET   0x0020
#define FCR_RESET   0x0000
#define LSR_RESET   0x0000
#define SEMR_RESET  0x00
#define FTCR_RESET  0x1f1f

/*
 * Start state for "direct-boot": what TF-A's console_rz_init() leaves
 * behind (BRR 0x1a, then SEMR = MDDRS | BRME and MDDR 0xfe, SCR = TE |
 * RE), with FSR showing an idle transmitter. All but BRR were read back
 * on an RZ/V2L board; BRR is hidden behind MDDR while MDDRS = 1.
 */
#define BRR_DIRECT  0x1a
#define MDDR_DIRECT 0xfe
#define SCR_DIRECT  0x0030
#define FSR_DIRECT  0x0060
#define SEMR_DIRECT 0x30

typedef struct {
    hwaddr offset;
    unsigned size;
} RZSCIFAReg;

static const RZSCIFAReg rz_scifa_regs[] = {
    { A_SMR, 2 }, { A_BRR, 1 }, { A_SCR, 2 }, { A_FTDR, 1 },
    { A_FSR, 2 }, { A_FRDR, 1 }, { A_FCR, 2 }, { A_FDR, 2 },
    { A_SPTR, 2 }, { A_LSR, 2 }, { A_SEMR, 1 }, { A_FTCR, 2 },
};

typedef enum {
    RZ_SCIFA_ACCESS_OK,         /* a whole register at its width */
    RZ_SCIFA_ACCESS_BAD,        /* part of a register, or the wrong width */
    RZ_SCIFA_ACCESS_HOLE,       /* no register at these bytes */
} RZSCIFAAccess;

static RZSCIFAAccess rz_scifa_classify(hwaddr offset, unsigned size)
{
    for (int i = 0; i < ARRAY_SIZE(rz_scifa_regs); i++) {
        const RZSCIFAReg *r = &rz_scifa_regs[i];

        if (offset < r->offset + r->size && r->offset < offset + size) {
            return (offset == r->offset && size == r->size) ?
                   RZ_SCIFA_ACCESS_OK : RZ_SCIFA_ACCESS_BAD;
        }
    }
    return RZ_SCIFA_ACCESS_HOLE;
}

/* Set FSR flags as the hardware does: a fresh set is not yet read */
static void rz_scifa_set_flags(RenesasRZSCIFAState *s, uint16_t flags)
{
    s->fsr_armed &= ~(flags & ~s->fsr);
    s->fsr |= flags;
}

static void rz_scifa_clear_flags(RenesasRZSCIFAState *s, uint16_t flags)
{
    s->fsr &= ~flags;
    s->fsr_armed &= ~flags;
}

/* Transmit trigger: FCR.TTRG, or FTCR.TFTC when TTRGS is set */
static unsigned rz_scifa_tx_trigger(RenesasRZSCIFAState *s)
{
    static const uint8_t ttrg[] = { 8, 4, 2, 0 };

    if (FIELD_EX16(s->ftcr, FTCR, TTRGS)) {
        return FIELD_EX16(s->ftcr, FTCR, TFTC);
    }
    return ttrg[FIELD_EX16(s->fcr, FCR, TTRG)];
}

/* Receive trigger: FCR.RTRG, or FTCR.RFTC when RTRGS is set */
static unsigned rz_scifa_rx_trigger(RenesasRZSCIFAState *s)
{
    static const uint8_t rtrg[] = { 1, 4, 8, 14 };

    if (FIELD_EX16(s->ftcr, FTCR, RTRGS)) {
        return FIELD_EX16(s->ftcr, FTCR, RFTC);
    }
    return rtrg[FIELD_EX16(s->fcr, FCR, RTRG)];
}

/* Re-evaluate the level flags after any change */
static void rz_scifa_update_flags(RenesasRZSCIFAState *s)
{
    if (!FIELD_EX16(s->scr, SCR, TE) ||
        fifo8_num_used(&s->tx_fifo) <= rz_scifa_tx_trigger(s)) {
        rz_scifa_set_flags(s, R_FSR_TDFE_MASK);
    }
    /* after a clear, RDF is set again while the FIFO exceeds the trigger */
    if (fifo8_num_used(&s->rx_fifo) > rz_scifa_rx_trigger(s)) {
        rz_scifa_set_flags(s, R_FSR_RDF_MASK);
    }
}

/* Interrupt request lines, Table 22.19 */
static void rz_scifa_update_irq(RenesasRZSCIFAState *s)
{
    bool tie = FIELD_EX16(s->scr, SCR, TIE);
    bool rie = FIELD_EX16(s->scr, SCR, RIE);
    bool reie = FIELD_EX16(s->scr, SCR, REIE);
    bool teie = FIELD_EX16(s->scr, SCR, TEIE);
    bool er = FIELD_EX16(s->fsr, FSR, ER);
    bool brk = FIELD_EX16(s->fsr, FSR, BRK);
    bool orer = FIELD_EX16(s->lsr, LSR, ORER);

    qemu_set_irq(s->irq[RENESAS_RZ_SCIFA_ERI], (rie || reie) && er);
    qemu_set_irq(s->irq[RENESAS_RZ_SCIFA_BRI], (rie || reie) && (brk || orer));
    qemu_set_irq(s->irq[RENESAS_RZ_SCIFA_RXI],
                 rie && FIELD_EX16(s->fsr, FSR, RDF));
    qemu_set_irq(s->irq[RENESAS_RZ_SCIFA_TXI],
                 tie && FIELD_EX16(s->fsr, FSR, TDFE));
    qemu_set_irq(s->irq[RENESAS_RZ_SCIFA_TEI_DRI],
                 (teie && FIELD_EX16(s->fsr, FSR, TEND)) ||
                 (rie && FIELD_EX16(s->fsr, FSR, DR)));
}

static void rz_scifa_update(RenesasRZSCIFAState *s)
{
    rz_scifa_update_flags(s);
    rz_scifa_update_irq(s);
}

/*
 * The time of `bits` bit periods on the line, in ns, at the bit rate of
 * the section 22.2.8 formula, corrected by MDDR / 256 when SEMR.BRME is
 * set (section 22.2.9).
 */
static uint64_t rz_scifa_bits_ns(RenesasRZSCIFAState *s, unsigned bits)
{
    unsigned n = FIELD_EX16(s->smr, SMR, CKS);
    /* base clock 16 or 8 times the bit rate, halved in double-speed mode */
    unsigned k = 64 >> (FIELD_EX8(s->semr, SEMR, ABCS0) +
                        FIELD_EX8(s->semr, SEMR, BGDM));
    /* bit time = k * 2^(2n-1) * (N + 1) / P0, times 256 / MDDR */
    uint64_t num = (uint64_t)k * (1u << (2 * n)) * (s->brr + 1) *
                   NANOSECONDS_PER_SECOND * bits;
    uint64_t den = 2 * RZ_SCIFA_P0_HZ;

    if (FIELD_EX8(s->semr, SEMR, BRME)) {
        num *= 256;
        den *= s->mddr;
    }
    return num / den;
}

/* One frame: start bit, 7 or 8 data bits, parity, 1 or 2 stop bits */
static uint64_t rz_scifa_frame_ns(RenesasRZSCIFAState *s)
{
    return rz_scifa_bits_ns(s, 1 + (FIELD_EX16(s->smr, SMR, CHR) ? 7 : 8) +
                            FIELD_EX16(s->smr, SMR, PE) +
                            (FIELD_EX16(s->smr, SMR, STOP) ? 2 : 1));
}

/* A received byte, at the end of its stop bit */
static void rz_scifa_rx_byte(RenesasRZSCIFAState *s, uint8_t byte)
{
    if (!FIELD_EX16(s->scr, SCR, RE) || FIELD_EX16(s->fcr, FCR, RFRST)) {
        return;
    }
    /* an overrun stops the receiver until ORER is cleared */
    if (s->lsr & R_LSR_ORER_MASK) {
        return;
    }
    if (fifo8_is_full(&s->rx_fifo)) {
        if (!(s->lsr & R_LSR_ORER_MASK)) {
            s->lsr |= R_LSR_ORER_MASK;
            s->lsr_armed &= ~R_LSR_ORER_MASK;
        }
        return;
    }
    fifo8_push(&s->rx_fifo, byte);
    if (fifo8_num_used(&s->rx_fifo) >= rz_scifa_rx_trigger(s)) {
        rz_scifa_set_flags(s, R_FSR_RDF_MASK);
    }
    /* DR: 15 bit times (ETU) after the last stop bit (section 22.2.7) */
    timer_mod(s->dr_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + rz_scifa_bits_ns(s, 15));
    rz_scifa_update(s);
}

static void rz_scifa_dr_timer_cb(void *opaque)
{
    RenesasRZSCIFAState *s = opaque;
    unsigned count = fifo8_num_used(&s->rx_fifo);

    if (count && count < rz_scifa_rx_trigger(s)) {
        rz_scifa_set_flags(s, R_FSR_DR_MASK);
    }
    rz_scifa_update_irq(s);
}

static int rz_scifa_can_receive(void *opaque)
{
    RenesasRZSCIFAState *s = opaque;

    if (!FIELD_EX16(s->scr, SCR, RE) || FIELD_EX16(s->fcr, FCR, RFRST) ||
        (s->lsr & R_LSR_ORER_MASK)) {
        return 1;       /* the line is not sampled: the byte is lost */
    }
    return fifo8_num_free(&s->rx_fifo);
}

static void rz_scifa_receive(void *opaque, const uint8_t *buf, int size)
{
    RenesasRZSCIFAState *s = opaque;

    for (int i = 0; i < size; i++) {
        rz_scifa_rx_byte(s, buf[i]);
    }
}

static uint8_t rz_scifa_frdr_read(RenesasRZSCIFAState *s)
{
    uint8_t byte = 0;

    if (fifo8_is_empty(&s->rx_fifo)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: FRDR read with an empty FIFO\n", __func__);
    } else {
        byte = fifo8_pop(&s->rx_fifo);
        qemu_chr_fe_accept_input(&s->chr);
    }
    rz_scifa_clear_flags(s, R_FSR_RDF_MASK);
    if (fifo8_is_empty(&s->rx_fifo)) {
        rz_scifa_clear_flags(s, R_FSR_DR_MASK);
    }
    rz_scifa_update(s);
    return byte;
}

static void rz_scifa_tx_complete(RenesasRZSCIFAState *s);

static gboolean rz_scifa_tx_watch_cb(void *do_not_use, GIOCondition cond,
                                     void *opaque)
{
    RenesasRZSCIFAState *s = opaque;

    s->tx_watch = 0;
    if (qemu_chr_fe_write(&s->chr, &s->tsr, 1) == 1) {
        s->tx_sent = true;
        if (s->tx_frame_done) {
            rz_scifa_tx_complete(s);
        }
        return G_SOURCE_REMOVE;
    }
    s->tx_watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                        rz_scifa_tx_watch_cb, s);
    if (!s->tx_watch) {
        /* the backend went away: the byte is lost on the line */
        s->tx_sent = true;
        if (s->tx_frame_done) {
            rz_scifa_tx_complete(s);
        }
    }
    return G_SOURCE_REMOVE;
}

static void rz_scifa_tx_send(RenesasRZSCIFAState *s)
{
    /* in loopback the receiver gets the byte when its frame is done */
    if (FIELD_EX16(s->fcr, FCR, LOOP) ||
        !qemu_chr_fe_backend_connected(&s->chr) ||
        qemu_chr_fe_write(&s->chr, &s->tsr, 1) == 1) {
        s->tx_sent = true;
        return;
    }
    s->tx_watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                        rz_scifa_tx_watch_cb, s);
    s->tx_sent = !s->tx_watch;
}

/* Move the next byte into the shift register, if one can go */
static void rz_scifa_tx_start(RenesasRZSCIFAState *s)
{
    if (s->tx_busy || !FIELD_EX16(s->scr, SCR, TE) ||
        fifo8_is_empty(&s->tx_fifo)) {
        return;
    }
    s->tsr = fifo8_pop(&s->tx_fifo);
    s->tx_busy = true;
    s->tx_frame_done = false;
    rz_scifa_tx_send(s);
    timer_mod(s->tx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + rz_scifa_frame_ns(s));
}

/* The frame in the shift register is out and has reached the chardev */
static void rz_scifa_tx_complete(RenesasRZSCIFAState *s)
{
    if (FIELD_EX16(s->fcr, FCR, LOOP)) {
        rz_scifa_rx_byte(s, s->tsr);
    }
    s->tx_busy = false;
    s->tx_frame_done = false;
    rz_scifa_tx_start(s);
    /* TEND needs an empty FIFO, not just an idle shift register */
    if (!s->tx_busy && fifo8_is_empty(&s->tx_fifo)) {
        rz_scifa_set_flags(s, R_FSR_TEND_MASK);
    }
    rz_scifa_update(s);
}

static void rz_scifa_tx_timer_cb(void *opaque)
{
    RenesasRZSCIFAState *s = opaque;

    s->tx_frame_done = true;
    if (s->tx_sent) {
        rz_scifa_tx_complete(s);
    }
}

static void rz_scifa_ftdr_write(RenesasRZSCIFAState *s, uint8_t value)
{
    if (FIELD_EX16(s->fcr, FCR, TFRST)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: FTDR write ignored during FIFO reset\n", __func__);
        return;
    }
    if (fifo8_is_full(&s->tx_fifo)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: FTDR write to a full FIFO discarded\n", __func__);
        return;
    }
    fifo8_push(&s->tx_fifo, value);
    rz_scifa_clear_flags(s, R_FSR_TEND_MASK | R_FSR_TDFE_MASK);
    rz_scifa_tx_start(s);
    rz_scifa_update(s);
}

static uint16_t rz_scifa_sptr_read(RenesasRZSCIFAState *s)
{
    uint16_t pins = R_SPTR_SPB2DT_MASK;   /* RXD idle (mark) */

    return (s->sptr & ~SPTR_PIN_BITS) | pins;
}

static MemTxResult rz_scifa_read(void *opaque, hwaddr offset,
                                 uint64_t *data, unsigned size,
                                 MemTxAttrs attrs)
{
    RenesasRZSCIFAState *s = RENESAS_RZ_SCIFA(opaque);
    uint64_t r;

    switch (rz_scifa_classify(offset, size)) {
    case RZ_SCIFA_ACCESS_BAD:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad %u-byte read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, size, offset);
        return MEMTX_DECODE_ERROR;
    case RZ_SCIFA_ACCESS_HOLE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read at undefined offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        *data = 0;
        return MEMTX_OK;
    case RZ_SCIFA_ACCESS_OK:
        break;
    }

    switch (offset) {
    case A_SMR:
        r = s->smr;
        break;
    case A_BRR:
        r = FIELD_EX8(s->semr, SEMR, MDDRS) ? s->mddr : s->brr;
        break;
    case A_SCR:
        r = s->scr;
        break;
    case A_FSR:
        r = s->fsr;
        s->fsr_armed |= s->fsr & FSR_CLEARABLE;
        break;
    case A_FCR:
        r = s->fcr;
        break;
    case A_FDR:
        r = FIELD_DP16(0, FDR, T, fifo8_num_used(&s->tx_fifo)) |
            FIELD_DP16(0, FDR, R, fifo8_num_used(&s->rx_fifo));
        break;
    case A_SPTR:
        r = rz_scifa_sptr_read(s);
        break;
    case A_LSR:
        r = s->lsr;
        s->lsr_armed |= s->lsr & R_LSR_ORER_MASK;
        break;
    case A_SEMR:
        r = s->semr;
        break;
    case A_FTCR:
        r = s->ftcr;
        break;
    case A_FRDR:
        r = rz_scifa_frdr_read(s);
        break;
    case A_FTDR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read of write-only FTDR\n", __func__);
        r = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }

    trace_renesas_rz_scifa_read(offset, r, size);
    *data = r;
    return MEMTX_OK;
}

static MemTxResult rz_scifa_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size,
                                  MemTxAttrs attrs)
{
    RenesasRZSCIFAState *s = RENESAS_RZ_SCIFA(opaque);
    uint16_t clr;

    switch (rz_scifa_classify(offset, size)) {
    case RZ_SCIFA_ACCESS_BAD:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad %u-byte write at offset 0x%" HWADDR_PRIx "\n",
                      __func__, size, offset);
        return MEMTX_DECODE_ERROR;
    case RZ_SCIFA_ACCESS_HOLE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write at undefined offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_OK;
    case RZ_SCIFA_ACCESS_OK:
        break;
    }

    trace_renesas_rz_scifa_write(offset, value, size);

    switch (offset) {
    case A_SMR:
        if (FIELD_EX16(value, SMR, CM)) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: clock synchronous mode not supported\n",
                          __func__);
            return MEMTX_DECODE_ERROR;
        }
        s->smr = value & SMR_WMASK;
        break;
    case A_BRR:
        if (!FIELD_EX8(s->semr, SEMR, MDDRS)) {
            s->brr = value;
        } else if (FIELD_EX16(s->scr, SCR, TE) ||
                   FIELD_EX16(s->scr, SCR, RE)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: MDDR write ignored while TE or RE is set\n",
                          __func__);
        } else {
            s->mddr = value | MDDR_FIXED;
        }
        break;
    case A_SCR:
        s->scr = value & SCR_WMASK;
        rz_scifa_tx_start(s);
        break;
    case A_FTDR:
        rz_scifa_ftdr_write(s, value);
        break;
    case A_FSR:
        /* a 0 clears a flag only if it was read as 1; 1s do nothing */
        clr = ~value & s->fsr_armed & FSR_CLEARABLE;
        rz_scifa_clear_flags(s, clr);
        break;
    case A_FCR:
        s->fcr = value & FCR_WMASK;
        if (FIELD_EX16(s->fcr, FCR, TFRST)) {
            fifo8_reset(&s->tx_fifo);
        }
        if (FIELD_EX16(s->fcr, FCR, RFRST)) {
            fifo8_reset(&s->rx_fifo);
            timer_del(s->dr_timer);
        }
        rz_scifa_tx_start(s);
        break;
    case A_FDR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only FDR\n", __func__);
        break;
    case A_SPTR:
        s->sptr = value & SPTR_WMASK;
        break;
    case A_LSR:
        /* only ORER is writable, with the same read-1-then-write-0 rule */
        if (!(value & R_LSR_ORER_MASK) && (s->lsr_armed & R_LSR_ORER_MASK)) {
            s->lsr &= ~R_LSR_ORER_MASK;
            s->lsr_armed &= ~R_LSR_ORER_MASK;
        }
        break;
    case A_FRDR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only FRDR\n", __func__);
        break;
    case A_SEMR:
        s->semr = value & SEMR_WMASK;
        break;
    case A_FTCR:
        s->ftcr = value & FTCR_WMASK;
        if ((FIELD_EX16(s->ftcr, FTCR, TTRGS) &&
             FIELD_EX16(s->ftcr, FTCR, TFTC) > 0x0f) ||
            (FIELD_EX16(s->ftcr, FTCR, RTRGS) &&
             (FIELD_EX16(s->ftcr, FTCR, RFTC) < 0x01 ||
              FIELD_EX16(s->ftcr, FTCR, RFTC) > 0x10))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: FTCR trigger number out of range\n", __func__);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }
    rz_scifa_update(s);
    return MEMTX_OK;
}

static const MemoryRegionOps rz_scifa_ops = {
    .read_with_attrs = rz_scifa_read,
    .write_with_attrs = rz_scifa_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static void rz_scifa_reset_hold(Object *obj, ResetType type)
{
    RenesasRZSCIFAState *s = RENESAS_RZ_SCIFA(obj);

    s->smr = SMR_RESET;
    s->brr = BRR_RESET;
    s->mddr = MDDR_RESET;
    s->scr = SCR_RESET;
    s->fsr = FSR_RESET;
    s->fsr_armed = 0;
    s->fcr = FCR_RESET;
    s->sptr = 0;
    s->lsr = LSR_RESET;
    s->lsr_armed = 0;
    s->semr = SEMR_RESET;
    s->ftcr = FTCR_RESET;

    fifo8_reset(&s->tx_fifo);
    timer_del(s->tx_timer);
    if (s->tx_watch) {
        g_source_remove(s->tx_watch);
        s->tx_watch = 0;
    }
    s->tx_busy = false;
    s->tx_sent = false;
    s->tx_frame_done = false;
    fifo8_reset(&s->rx_fifo);
    timer_del(s->dr_timer);

    if (s->direct_boot) {
        s->brr = BRR_DIRECT;
        s->mddr = MDDR_DIRECT;
        s->scr = SCR_DIRECT;
        s->fsr = FSR_DIRECT;
        s->semr = SEMR_DIRECT;
    }
    rz_scifa_update_flags(s);
}

static void rz_scifa_reset_exit(Object *obj, ResetType type)
{
    rz_scifa_update_irq(RENESAS_RZ_SCIFA(obj));
}

static void rz_scifa_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RenesasRZSCIFAState *s = RENESAS_RZ_SCIFA(obj);

    memory_region_init_io(&s->iomem, obj, &rz_scifa_ops, s,
                          TYPE_RENESAS_RZ_SCIFA, RENESAS_RZ_SCIFA_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    for (int i = 0; i < RENESAS_RZ_SCIFA_NUM_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
    fifo8_create(&s->tx_fifo, RENESAS_RZ_SCIFA_FIFO_SIZE);
    s->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rz_scifa_tx_timer_cb, s);
    fifo8_create(&s->rx_fifo, RENESAS_RZ_SCIFA_FIFO_SIZE);
    s->dr_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rz_scifa_dr_timer_cb, s);
}

static void rz_scifa_realize(DeviceState *dev, Error **errp)
{
    RenesasRZSCIFAState *s = RENESAS_RZ_SCIFA(dev);

    qemu_chr_fe_set_handlers(&s->chr, rz_scifa_can_receive, rz_scifa_receive,
                             NULL, NULL, s, NULL, true);
}

static void rz_scifa_finalize(Object *obj)
{
    RenesasRZSCIFAState *s = RENESAS_RZ_SCIFA(obj);

    timer_free(s->tx_timer);
    fifo8_destroy(&s->tx_fifo);
    timer_free(s->dr_timer);
    fifo8_destroy(&s->rx_fifo);
}

static int rz_scifa_post_load(void *opaque, int version_id)
{
    RenesasRZSCIFAState *s = opaque;

    /* a byte that had not reached the chardev is retried */
    if (s->tx_busy && !s->tx_sent) {
        rz_scifa_tx_send(s);
    }
    return 0;
}

static const VMStateDescription vmstate_rz_scifa = {
    .name = TYPE_RENESAS_RZ_SCIFA,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rz_scifa_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(smr, RenesasRZSCIFAState),
        VMSTATE_UINT8(brr, RenesasRZSCIFAState),
        VMSTATE_UINT8(mddr, RenesasRZSCIFAState),
        VMSTATE_UINT16(scr, RenesasRZSCIFAState),
        VMSTATE_UINT16(fsr, RenesasRZSCIFAState),
        VMSTATE_UINT16(fsr_armed, RenesasRZSCIFAState),
        VMSTATE_UINT16(fcr, RenesasRZSCIFAState),
        VMSTATE_UINT16(sptr, RenesasRZSCIFAState),
        VMSTATE_UINT16(lsr, RenesasRZSCIFAState),
        VMSTATE_UINT16(lsr_armed, RenesasRZSCIFAState),
        VMSTATE_UINT8(semr, RenesasRZSCIFAState),
        VMSTATE_UINT16(ftcr, RenesasRZSCIFAState),
        VMSTATE_FIFO8(tx_fifo, RenesasRZSCIFAState),
        VMSTATE_UINT8(tsr, RenesasRZSCIFAState),
        VMSTATE_BOOL(tx_busy, RenesasRZSCIFAState),
        VMSTATE_BOOL(tx_sent, RenesasRZSCIFAState),
        VMSTATE_BOOL(tx_frame_done, RenesasRZSCIFAState),
        VMSTATE_TIMER_PTR(tx_timer, RenesasRZSCIFAState),
        VMSTATE_FIFO8(rx_fifo, RenesasRZSCIFAState),
        VMSTATE_TIMER_PTR(dr_timer, RenesasRZSCIFAState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property rz_scifa_props[] = {
    DEFINE_PROP_CHR("chardev", RenesasRZSCIFAState, chr),
    DEFINE_PROP_BOOL("direct-boot", RenesasRZSCIFAState, direct_boot, false),
};

static void rz_scifa_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = rz_scifa_reset_hold;
    rc->phases.exit = rz_scifa_reset_exit;
    dc->realize = rz_scifa_realize;
    dc->vmsd = &vmstate_rz_scifa;
    device_class_set_props(dc, rz_scifa_props);
}

static const TypeInfo rz_scifa_info = {
    .name = TYPE_RENESAS_RZ_SCIFA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZSCIFAState),
    .instance_init = rz_scifa_init,
    .instance_finalize = rz_scifa_finalize,
    .class_init = rz_scifa_class_init,
};

static void rz_scifa_register_types(void)
{
    type_register_static(&rz_scifa_info);
}

type_init(rz_scifa_register_types);
