/*
 * QTest testcase for the SCIFA channels of the Renesas RZ/V2L SMARC EVK
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Expected values come from the hardware documentation, never from the
 * model: the RZ/V2L Group User's Manual: Hardware (R01UH0936EJ0150),
 * section 22. The direct-boot values are the registers read back on an
 * RZ/V2L EVK at the U-Boot prompt, after the Renesas TF-A and U-Boot
 * have set the console up; they are marked as such.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* SCIF0-4 (manual Table 22.3) */
#define SCIF_BASE(n)    (0x1004b800 + (n) * 0x400)

#define SMR             0x00
#define BRR             0x02    /* MDDR while SEMR.MDDRS = 1 */
#define SCR             0x04
#define FSR             0x08
#define FCR             0x0c
#define FDR             0x0e
#define SPTR            0x10
#define LSR             0x12
#define SEMR            0x14
#define FTCR            0x16
#define UNDEFINED       0x18    /* first offset after FTCR */

#define SMR_CM          0x0080
#define SCR_TE          0x0020
#define SCR_RE          0x0010
#define SEMR_MDDRS      0x10
#define MDDR_B7         0x80    /* fixed to 1 (22.2.9) */
#define SPTR_SPB2DT     0x0001  /* reads the RXD pin (22.2.12) */
#define SPTR_CTRL_BITS  0x00aa  /* the bits that read back what was written */

/* Reset values, Table 22.4 (p.1271) */
#define SMR_RESET       0x0000
#define BRR_RESET       0xff
#define MDDR_RESET      0xff
#define SCR_RESET       0x0000
#define FSR_RESET       0x0020
#define FCR_RESET       0x0000
#define FDR_RESET       0x0000
#define LSR_RESET       0x0000
#define SEMR_RESET      0x00
#define FTCR_RESET      0x1f1f

/*
 * SMR (22.2.5, p.1274): bits 15-8 and bit 2 are reserved and read as 0;
 * bit 7 (CM) is written 0 here, since clock synchronous mode is refused.
 */
#define SMR_ALL_ONES_NO_CM  0xff7f
#define SMR_READBACK        0x007b

/*
 * Post-firmware state, read on the EVK at the U-Boot prompt
 * (docs: EVK post-firmware reads 2026-10-29, rows R20-R26). FSR with the
 * line idle. BRR cannot be read while MDDRS = 1; its value is derived
 * from TF-A console_rz_init(): 100 MHz / 115200 >> 5, minus 1.
 */
#define SMR_FW          0x0000
#define MDDR_FW         0xfe
#define BRR_FW          0x1a
#define SCR_FW          0x0030
#define FSR_FW          0x0060
#define FCR_FW          0x0000
#define FDR_FW          0x0000
#define SPTR_FW         0x0001
#define LSR_FW          0x0000
#define SEMR_FW         0x30
#define FTCR_FW         0x1f1f

#define FTDR            0x06
#define FSR_TEND        0x0040
#define FSR_TDFE        0x0020
#define FCR_TTRG(x)     ((x) << 4)
#define FTCR_TTRGS      0x0080
#define SMR_CHR         0x0040
#define SMR_PE          0x0020
#define SMR_STOP        0x0008

/*
 * Frame times at the direct-boot settings (BRR 0x1a, SEMR = MDDRS |
 * BRME, MDDR 0xfe, n = 0), from the bit-rate formulas of 22.2.8
 * (p.1282) and 22.2.9 (p.1287) with P0 = 100 MHz:
 *   bit time = 64 * 2^-1 * 27 / 100 MHz * 256 / 254 = 8.708 us
 * A 10-bit frame (start, 8 data, stop) takes 87.08 us; an 11-bit frame
 * (start, 7 data, parity, 2 stop) takes 95.79 us.
 */
#define FRDR            0x0a
#define FSR_RDF         0x0002
#define FSR_DR          0x0001
#define FCR_LOOP        0x0001
#define FCR_RTRG(x)     ((x) << 6)
#define LSR_ORER        0x0001
#define SCR_TE_ONLY     0x0020
#define SCR_TIE         0x0080
#define SCR_RIE         0x0040
#define SCR_REIE        0x0008
#define SCR_TEIE        0x0004

/* Interrupt outputs of a SCIF channel, in order (Table 22.19) */
enum { ERI, BRI, RXI, TXI, TEI_DRI };

#define FRAME_8N1_NS    87080
#define FRAME_7E2_NS    95790
/* DR: 15 bit times (ETU) after the last stop bit (22.2.7, p.1279) */
#define DR_DELAY_NS     130620
#define MARGIN_NS       200     /* below the MDDR correction, 680 ns */

/* With -kernel and no firmware, the board starts SCIF0 post-firmware */
static QTestState *start_direct(char **kernel)
{
    int fd = g_file_open_tmp("rzv2l-scifa-XXXXXX", kernel, NULL);

    g_assert(fd >= 0);
    close(fd);
    return qtest_initf("-machine rzv2l-smarc -kernel %s", *kernel);
}

static void stop_direct(QTestState *qts, char *kernel)
{
    qtest_quit(qts);
    unlink(kernel);
    g_free(kernel);
}

static void check_reset_state(QTestState *qts, int n)
{
    uint64_t b = SCIF_BASE(n);

    g_assert_cmphex(qtest_readw(qts, b + SMR), ==, SMR_RESET);
    g_assert_cmphex(qtest_readb(qts, b + BRR), ==, BRR_RESET);
    g_assert_cmphex(qtest_readw(qts, b + SCR), ==, SCR_RESET);
    g_assert_cmphex(qtest_readw(qts, b + FSR), ==, FSR_RESET);
    g_assert_cmphex(qtest_readw(qts, b + FCR), ==, FCR_RESET);
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, FDR_RESET);
    g_assert_cmphex(qtest_readw(qts, b + SPTR) & SPTR_CTRL_BITS, ==, 0);
    g_assert_cmphex(qtest_readw(qts, b + LSR), ==, LSR_RESET);
    g_assert_cmphex(qtest_readb(qts, b + SEMR), ==, SEMR_RESET);
    g_assert_cmphex(qtest_readw(qts, b + FTCR), ==, FTCR_RESET);

    /* MDDR shares offset 0x02 and is reached through SEMR.MDDRS */
    qtest_writeb(qts, b + SEMR, SEMR_MDDRS);
    g_assert_cmphex(qtest_readb(qts, b + BRR), ==, MDDR_RESET);
    qtest_writeb(qts, b + SEMR, SEMR_RESET);
}

static void test_reset(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    for (int n = 0; n < 5; n++) {
        check_reset_state(qts, n);
    }
    qtest_quit(qts);
}

static void test_brr_mddr_bank(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    uint64_t b = SCIF_BASE(0);

    /* MDDRS = 0: offset 0x02 is BRR, any value */
    qtest_writeb(qts, b + BRR, 0x35);
    g_assert_cmphex(qtest_readb(qts, b + BRR), ==, 0x35);

    /* MDDRS = 1: offset 0x02 is MDDR, which BRR's write did not reach */
    qtest_writeb(qts, b + SEMR, SEMR_MDDRS);
    g_assert_cmphex(qtest_readb(qts, b + BRR), ==, MDDR_RESET);
    qtest_writeb(qts, b + BRR, 0xc4);
    g_assert_cmphex(qtest_readb(qts, b + BRR), ==, 0xc4);

    /* back to MDDRS = 0: BRR kept its value */
    qtest_writeb(qts, b + SEMR, 0);
    g_assert_cmphex(qtest_readb(qts, b + BRR), ==, 0x35);
    qtest_quit(qts);
}

/* MDDR is writable only while SCR.TE = SCR.RE = 0 (22.2.9, p.1287) */
static void test_mddr_write_rule(void)
{
    static const uint16_t scr[] = { 0, SCR_TE, SCR_RE, SCR_TE | SCR_RE };
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    uint64_t b = SCIF_BASE(0);

    qtest_writeb(qts, b + SEMR, SEMR_MDDRS);
    for (int i = 0; i < ARRAY_SIZE(scr); i++) {
        uint8_t before, want;

        qtest_writew(qts, b + SCR, 0);
        qtest_writeb(qts, b + BRR, 0xa0 + i);
        before = qtest_readb(qts, b + BRR);
        g_assert_cmphex(before, ==, 0xa0 + i);

        qtest_writew(qts, b + SCR, scr[i]);
        qtest_writeb(qts, b + BRR, 0xd5);
        want = scr[i] ? before : 0xd5;
        g_assert_cmphex(qtest_readb(qts, b + BRR), ==, want);
    }

    /* bit 7 of MDDR is fixed to 1 */
    qtest_writew(qts, b + SCR, 0);
    qtest_writeb(qts, b + BRR, 0x12);
    g_assert_cmphex(qtest_readb(qts, b + BRR), ==, 0x12 | MDDR_B7);
    qtest_quit(qts);
}

static void test_smr_mask(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    uint64_t b = SCIF_BASE(0);

    qtest_writew(qts, b + SMR, SMR_ALL_ONES_NO_CM);
    g_assert_cmphex(qtest_readw(qts, b + SMR), ==, SMR_READBACK);
    qtest_quit(qts);
}

/*
 * Clock synchronous mode is not modelled: the SMR write that selects it
 * is refused (the guest takes a bus error, which qtest cannot see), so
 * SMR keeps its previous value.
 */
static void test_sync_mode_refused(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    uint64_t b = SCIF_BASE(0);

    qtest_writew(qts, b + SMR, 0x0008);
    qtest_writew(qts, b + SMR, SMR_CM | 0x0008);
    g_assert_cmphex(qtest_readw(qts, b + SMR), ==, 0x0008);
    qtest_quit(qts);
}

/* A 16-bit read at 0x18 read 0, without a bus error, on the EVK (R70) */
static void test_undefined_offset(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    g_assert_cmphex(qtest_readw(qts, SCIF_BASE(0) + UNDEFINED), ==, 0);
    qtest_quit(qts);
}

/*
 * With a kernel and no firmware, SCIF0 starts as the firmware leaves it;
 * SCIF1-4 are left unclocked by the firmware and start at reset.
 */
static void test_direct_boot(void)
{
    g_autofree char *kernel = NULL;
    QTestState *qts;
    uint64_t b = SCIF_BASE(0);
    int fd;

    fd = g_file_open_tmp("rzv2l-scifa-XXXXXX", &kernel, NULL);
    g_assert(fd >= 0);
    close(fd);
    qts = qtest_initf("-machine rzv2l-smarc -kernel %s", kernel);

    g_assert_cmphex(qtest_readw(qts, b + SMR), ==, SMR_FW);
    g_assert_cmphex(qtest_readb(qts, b + BRR), ==, MDDR_FW);
    g_assert_cmphex(qtest_readw(qts, b + SCR), ==, SCR_FW);
    g_assert_cmphex(qtest_readw(qts, b + FSR), ==, FSR_FW);
    g_assert_cmphex(qtest_readw(qts, b + FCR), ==, FCR_FW);
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, FDR_FW);
    g_assert_cmphex(qtest_readw(qts, b + SPTR), ==, SPTR_FW);
    g_assert_cmphex(qtest_readw(qts, b + LSR), ==, LSR_FW);
    g_assert_cmphex(qtest_readb(qts, b + SEMR), ==, SEMR_FW);
    g_assert_cmphex(qtest_readw(qts, b + FTCR), ==, FTCR_FW);

    /* BRR, behind MDDR until MDDRS is cleared */
    qtest_writeb(qts, b + SEMR, SEMR_FW & ~SEMR_MDDRS);
    g_assert_cmphex(qtest_readb(qts, b + BRR), ==, BRR_FW);

    for (int n = 1; n < 5; n++) {
        check_reset_state(qts, n);
    }
    qtest_quit(qts);
    unlink(kernel);
}

/* An FSR flag clears on a 0-write only after it was read as 1 (p.1278) */
static void test_fsr_read_armed(void)
{
    char *kernel;
    QTestState *qts = start_direct(&kernel);
    uint64_t b = SCIF_BASE(0);

    /* not read yet: the 0-write leaves TEND (and TDFE) set */
    qtest_writew(qts, b + FSR, 0);
    g_assert_cmphex(qtest_readw(qts, b + FSR), ==, FSR_FW);

    /* after the read the same write clears TEND; TDFE is set again */
    qtest_writew(qts, b + FSR, 0);
    g_assert_cmphex(qtest_readw(qts, b + FSR), ==, FSR_TDFE);
    stop_direct(qts, kernel);
}

/*
 * TDFE is set again right after a clear while the TX FIFO holds no more
 * bytes than the trigger (22.8.1, p.1327), and an FTDR write clears it
 * (p.1280). The first byte moves into the shift register at once, so
 * ten FTDR writes leave nine bytes in the FIFO (FDR.T, 22.2.11).
 */
static void test_tdfe_level(void)
{
    char *kernel;
    QTestState *qts = start_direct(&kernel);
    uint64_t b = SCIF_BASE(0);

    qtest_readw(qts, b + FSR);
    qtest_writew(qts, b + FSR, (uint16_t)~FSR_TDFE);
    g_assert_cmphex(qtest_readw(qts, b + FSR), ==, FSR_FW);

    for (int i = 0; i < 10; i++) {
        qtest_writeb(qts, b + FTDR, 'a' + i);
    }
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, 9 << 8);
    g_assert_cmphex(qtest_readw(qts, b + FSR), ==, 0);
    stop_direct(qts, kernel);
}

/* Write bytes until the FIFO holds `count`, then check TDFE */
static void check_tdfe_at(QTestState *qts, unsigned count, bool tdfe)
{
    uint64_t b = SCIF_BASE(0);

    /*
     * While the transmitter is idle (TEND = 1) the next byte goes to the
     * shift register, not the FIFO; after that each byte stays in the
     * FIFO, since no virtual time passes.
     */
    while ((qtest_readw(qts, b + FSR) & FSR_TEND) ||
           ((qtest_readw(qts, b + FDR) >> 8) & 0x1f) < count) {
        qtest_writeb(qts, b + FTDR, 'x');
    }
    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_TDFE, ==,
                    tdfe ? FSR_TDFE : 0);
}

/* TX trigger (22.2.10 p.1290, 22.2.15 p.1299): count <= trigger sets TDFE */
static void test_tx_trigger(void)
{
    static const struct {
        uint16_t fcr, ftcr;
        unsigned trigger;
    } cases[] = {
        { FCR_TTRG(1), 0x1f1f, 4 },             /* TTRG = 01: 4 */
        { FCR_TTRG(3), 0x1f1f, 0 },             /* TTRG = 11: 0 */
        { 0, 0x1f00 | FTCR_TTRGS | 2, 2 },      /* TTRGS, TFTC = 2 */
    };

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        char *kernel;
        QTestState *qts = start_direct(&kernel);
        uint64_t b = SCIF_BASE(0);

        qtest_writew(qts, b + FCR, cases[i].fcr);
        qtest_writew(qts, b + FTCR, cases[i].ftcr);
        check_tdfe_at(qts, cases[i].trigger, true);
        check_tdfe_at(qts, cases[i].trigger + 1, false);
        stop_direct(qts, kernel);
    }
}

/* An FTDR write clears TEND at once (22.2.7, p.1281) */
static void test_ftdr_clears_tend(void)
{
    char *kernel;
    QTestState *qts = start_direct(&kernel);
    uint64_t b = SCIF_BASE(0);

    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_TEND, ==, FSR_TEND);
    qtest_writeb(qts, b + FTDR, 'A');
    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_TEND, ==, 0);
    stop_direct(qts, kernel);
}

/*
 * TEND rises only when the last frame is out with the FIFO empty: one
 * frame time per byte at the programmed format and bit rate. Firmware
 * clears TEND right after the FTDR write and then waits for it.
 */
static void check_tend_after(QTestState *qts, unsigned bytes, uint64_t ns)
{
    uint64_t b = SCIF_BASE(0);

    for (unsigned i = 0; i < bytes; i++) {
        qtest_writeb(qts, b + FTDR, 'A' + i);
    }
    qtest_clock_step(qts, ns - MARGIN_NS);
    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_TEND, ==, 0);
    qtest_clock_step(qts, 2 * MARGIN_NS);
    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_TEND, ==, FSR_TEND);
}

static void test_tend_deferred(void)
{
    char *kernel;
    QTestState *qts = start_direct(&kernel);
    uint64_t b = SCIF_BASE(0);

    check_tend_after(qts, 1, FRAME_8N1_NS);
    check_tend_after(qts, 3, 3 * FRAME_8N1_NS);

    qtest_writew(qts, b + SMR, SMR_CHR | SMR_PE | SMR_STOP);
    check_tend_after(qts, 1, FRAME_7E2_NS);
    stop_direct(qts, kernel);
}

/*
 * TEND needs an empty FIFO, not just an idle shift register (22.2.7
 * p.1281: "set to 1 when the FTDR register does not contain transmit
 * data when the last bit ... is transmitted"). With TE cleared while a
 * byte is still queued, the frame in flight ends, nothing follows, and
 * TEND stays 0.
 */
static void test_tend_te_cleared(void)
{
    char *kernel;
    QTestState *qts = start_direct(&kernel);
    uint64_t b = SCIF_BASE(0);

    qtest_writeb(qts, b + FTDR, 'A');
    qtest_writeb(qts, b + FTDR, 'B');
    qtest_writew(qts, b + SCR, SCR_RE);
    qtest_clock_step(qts, 2 * FRAME_8N1_NS + MARGIN_NS);
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, 0x0100);
    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_TEND, ==, 0);
    stop_direct(qts, kernel);
}

/*
 * Loopback (FCR.LOOP) feeds the receiver from the transmitter; a looped
 * byte arrives when its frame is done, so the receive timing is exact.
 */
static void loop_bytes(QTestState *qts, const char *str, uint64_t frame)
{
    uint64_t b = SCIF_BASE(0);

    for (const char *c = str; *c; c++) {
        qtest_writeb(qts, b + FTDR, *c);
    }
    qtest_clock_step(qts, strlen(str) * frame + MARGIN_NS);
}

/*
 * RX FIFO, FDR.R, RDF and FRDR (22.2.7 p.1279, 22.8.2 p.1327): RDF sets
 * when the FIFO reaches the trigger; an FRDR read pops and clears RDF;
 * RDF is set again only while the FIFO holds MORE than the trigger.
 */
static void test_rx_fifo(void)
{
    char *kernel;
    QTestState *qts = start_direct(&kernel);
    uint64_t b = SCIF_BASE(0);

    qtest_writew(qts, b + FCR, FCR_LOOP | FCR_RTRG(1));     /* trigger 4 */
    loop_bytes(qts, "abcdef", FRAME_8N1_NS);
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, 6);
    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_RDF, ==, FSR_RDF);

    g_assert_cmphex(qtest_readb(qts, b + FRDR), ==, 'a');   /* 5 left */
    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_RDF, ==, FSR_RDF);
    g_assert_cmphex(qtest_readb(qts, b + FRDR), ==, 'b');   /* 4 left */
    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_RDF, ==, 0);
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, 4);
    stop_direct(qts, kernel);
}

/* Bytes from the serial line reach FRDR in order */
static void test_rx_chardev(void)
{
    g_autofree char *kernel = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("rzv2l-scifa-XXXXXX", &kernel, NULL);
    uint64_t b = SCIF_BASE(0);
    QTestState *qts;
    int sock;

    g_assert(fd >= 0);
    close(fd);
    args = g_strdup_printf("-machine rzv2l-smarc -kernel %s", kernel);
    qts = qtest_init_with_serial(args, &sock);
    g_assert_true(send(sock, "xyz", 3, 0) == 3);
    /* the bytes arrive through QEMU's main loop: wait up to 5 s */
    for (int i = 0; i < 5000 && (qtest_readw(qts, b + FDR) & 0x1f) < 3; i++) {
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, 3);
    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_RDF, ==, FSR_RDF);
    g_assert_cmphex(qtest_readb(qts, b + FRDR), ==, 'x');
    g_assert_cmphex(qtest_readb(qts, b + FRDR), ==, 'y');
    g_assert_cmphex(qtest_readb(qts, b + FRDR), ==, 'z');
    close(sock);
    qtest_quit(qts);
    unlink(kernel);
}

/*
 * DR sets 15 bit times after the last byte while the FIFO holds fewer
 * bytes than the trigger, whatever the frame format (in 7E2 that is not
 * 1.5 frames), and clears when the FIFO is drained.
 */
static void check_dr(QTestState *qts, uint64_t frame)
{
    uint64_t b = SCIF_BASE(0);

    qtest_writeb(qts, b + FTDR, 'q');
    qtest_clock_step(qts, frame + DR_DELAY_NS - MARGIN_NS);
    g_assert_cmphex(qtest_readw(qts, b + FSR) & (FSR_DR | FSR_RDF), ==, 0);
    qtest_clock_step(qts, 2 * MARGIN_NS);
    g_assert_cmphex(qtest_readw(qts, b + FSR) & (FSR_DR | FSR_RDF), ==,
                    FSR_DR);
    g_assert_cmphex(qtest_readb(qts, b + FRDR), ==, 'q');
    g_assert_cmphex(qtest_readw(qts, b + FSR) & FSR_DR, ==, 0);
}

static void test_rx_dr_timeout(void)
{
    char *kernel;
    QTestState *qts = start_direct(&kernel);
    uint64_t b = SCIF_BASE(0);

    qtest_writew(qts, b + FCR, FCR_LOOP | FCR_RTRG(1));     /* trigger 4 */
    check_dr(qts, FRAME_8N1_NS);
    qtest_writew(qts, b + SMR, SMR_CHR | SMR_PE | SMR_STOP);
    check_dr(qts, FRAME_7E2_NS);
    stop_direct(qts, kernel);
}

/*
 * A byte that finds the RX FIFO full is lost and sets LSR.ORER
 * (22.2.13, p.1296), which clears on a 0-write only after a 1-read.
 * Loopback can overrun: 17 bytes in flight (16 in the TX FIFO, one in
 * the shift register) into a 16-byte RX FIFO nobody reads.
 */
static void test_rx_overrun(void)
{
    char *kernel;
    QTestState *qts = start_direct(&kernel);
    uint64_t b = SCIF_BASE(0);

    qtest_writew(qts, b + FCR, FCR_LOOP);
    loop_bytes(qts, "0123456789abcdefg", FRAME_8N1_NS);
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, 16);
    qtest_writew(qts, b + LSR, 0);
    g_assert_cmphex(qtest_readw(qts, b + LSR), ==, LSR_ORER);
    qtest_writew(qts, b + LSR, 0);
    g_assert_cmphex(qtest_readw(qts, b + LSR), ==, 0);
    stop_direct(qts, kernel);
}

/*
 * While ORER is set the receiver is stopped (22.2.13 p.1296: "the SCIFA
 * cannot continue subsequent serial reception"; 22.4.2, the overrun
 * check before a byte is stored). Freeing a FIFO slot does not restart
 * it; clearing ORER does.
 */
static void test_rx_stopped_by_orer(void)
{
    char *kernel;
    QTestState *qts = start_direct(&kernel);
    uint64_t b = SCIF_BASE(0);

    qtest_writew(qts, b + FCR, FCR_LOOP);
    loop_bytes(qts, "0123456789abcdefg", FRAME_8N1_NS);
    g_assert_cmphex(qtest_readb(qts, b + FRDR), ==, '0');
    loop_bytes(qts, "h", FRAME_8N1_NS);
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, 15);
    g_assert_cmphex(qtest_readw(qts, b + LSR), ==, LSR_ORER);
    qtest_writew(qts, b + LSR, 0);
    loop_bytes(qts, "i", FRAME_8N1_NS);
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, 16);
    stop_direct(qts, kernel);
}

/* With RE = 0 the receiver ignores the line */
static void test_rx_disabled(void)
{
    char *kernel;
    QTestState *qts = start_direct(&kernel);
    uint64_t b = SCIF_BASE(0);

    qtest_writew(qts, b + SCR, SCR_TE_ONLY);
    qtest_writew(qts, b + FCR, FCR_LOOP);
    loop_bytes(qts, "r", FRAME_8N1_NS);
    g_assert_cmphex(qtest_readw(qts, b + FDR), ==, 0);
    stop_direct(qts, kernel);
}

/* Start SCIF0 post-firmware with its interrupt outputs intercepted */
static QTestState *start_irq(char **kernel)
{
    QTestState *qts = start_direct(kernel);

    qtest_irq_intercept_out_named(qts, "/machine/soc/scif0", "sysbus-irq");
    /*
     * qtest learns a line's level only when the device drives it after
     * the intercept; rewrite SCR unchanged so all five lines report.
     */
    qtest_writew(qts, SCIF_BASE(0) + SCR, SCR_FW);
    return qts;
}

static void check_lines(QTestState *qts, bool eri, bool bri, bool rxi,
                        bool txi, bool tei_dri)
{
    g_assert_cmpint(qtest_get_irq(qts, ERI), ==, eri);
    g_assert_cmpint(qtest_get_irq(qts, BRI), ==, bri);
    g_assert_cmpint(qtest_get_irq(qts, RXI), ==, rxi);
    g_assert_cmpint(qtest_get_irq(qts, TXI), ==, txi);
    g_assert_cmpint(qtest_get_irq(qts, TEI_DRI), ==, tei_dri);
}

/* TXI = TIE & TDFE */
static void test_irq_txi(void)
{
    char *kernel;
    QTestState *qts = start_irq(&kernel);
    uint64_t b = SCIF_BASE(0);

    check_lines(qts, 0, 0, 0, 0, 0);                    /* TDFE = 1, TIE = 0 */
    qtest_writew(qts, b + SCR, SCR_FW | SCR_TIE);
    check_lines(qts, 0, 0, 0, 1, 0);
    for (int i = 0; i < 10; i++) {                      /* 9 in the FIFO > 8 */
        qtest_writeb(qts, b + FTDR, 'a' + i);
    }
    check_lines(qts, 0, 0, 0, 0, 0);
    stop_direct(qts, kernel);
}

/* RXI = RIE & RDF */
static void test_irq_rxi(void)
{
    char *kernel;
    QTestState *qts = start_irq(&kernel);
    uint64_t b = SCIF_BASE(0);

    qtest_writew(qts, b + FCR, FCR_LOOP);               /* trigger 1 */
    loop_bytes(qts, "r", FRAME_8N1_NS);
    check_lines(qts, 0, 0, 0, 0, 0);                    /* RDF = 1, RIE = 0 */
    qtest_writew(qts, b + SCR, SCR_FW | SCR_RIE);
    check_lines(qts, 0, 0, 1, 0, 0);
    qtest_readb(qts, b + FRDR);                         /* clears RDF */
    check_lines(qts, 0, 0, 0, 0, 0);
    stop_direct(qts, kernel);
}

/* TEI/DRI = (TEIE & TEND) | (RIE & DR), each term on its own */
static void test_irq_tei_dri(void)
{
    char *kernel;
    QTestState *qts = start_irq(&kernel);
    uint64_t b = SCIF_BASE(0);

    qtest_writew(qts, b + SCR, SCR_FW | SCR_TEIE);      /* TEND = 1 */
    check_lines(qts, 0, 0, 0, 0, 1);
    qtest_writeb(qts, b + FTDR, 't');                   /* TEND = 0 */
    check_lines(qts, 0, 0, 0, 0, 0);
    qtest_clock_step(qts, FRAME_8N1_NS + MARGIN_NS);
    check_lines(qts, 0, 0, 0, 0, 1);

    /* DR term: one looped byte below the trigger, RIE set, TEIE clear */
    qtest_writew(qts, b + SCR, SCR_FW | SCR_RIE);
    qtest_writew(qts, b + FCR, FCR_LOOP | FCR_RTRG(1)); /* trigger 4 */
    qtest_writeb(qts, b + FTDR, 'd');
    qtest_clock_step(qts, FRAME_8N1_NS + DR_DELAY_NS - MARGIN_NS);
    check_lines(qts, 0, 0, 0, 0, 0);
    qtest_clock_step(qts, 2 * MARGIN_NS);
    check_lines(qts, 0, 0, 0, 0, 1);
    stop_direct(qts, kernel);
}

/*
 * BRI = (RIE | REIE) & (BRK | ORER), shown through a loopback overrun
 * with REIE alone; ERI = (RIE | REIE) & ER stays low, since no backend
 * produces ER.
 */
static void test_irq_bri(void)
{
    char *kernel;
    QTestState *qts = start_irq(&kernel);
    uint64_t b = SCIF_BASE(0);

    qtest_writew(qts, b + FCR, FCR_LOOP);
    loop_bytes(qts, "0123456789abcdefg", FRAME_8N1_NS);
    check_lines(qts, 0, 0, 0, 0, 0);                    /* ORER, REIE = 0 */
    qtest_writew(qts, b + SCR, SCR_FW | SCR_REIE);
    check_lines(qts, 0, 1, 0, 0, 0);
    qtest_readw(qts, b + LSR);
    qtest_writew(qts, b + LSR, 0);                      /* clears ORER */
    check_lines(qts, 0, 0, 0, 0, 0);
    stop_direct(qts, kernel);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rzv2l-smarc/scif/reset", test_reset);
    qtest_add_func("/rzv2l-smarc/scif/brr-mddr-bank", test_brr_mddr_bank);
    qtest_add_func("/rzv2l-smarc/scif/mddr-write-rule",
                   test_mddr_write_rule);
    qtest_add_func("/rzv2l-smarc/scif/smr-mask", test_smr_mask);
    qtest_add_func("/rzv2l-smarc/scif/sync-mode-refused",
                   test_sync_mode_refused);
    qtest_add_func("/rzv2l-smarc/scif/undefined-offset",
                   test_undefined_offset);
    qtest_add_func("/rzv2l-smarc/scif/direct-boot", test_direct_boot);
    qtest_add_func("/rzv2l-smarc/scif/fsr-read-armed", test_fsr_read_armed);
    qtest_add_func("/rzv2l-smarc/scif/tdfe-level", test_tdfe_level);
    qtest_add_func("/rzv2l-smarc/scif/tx-trigger", test_tx_trigger);
    qtest_add_func("/rzv2l-smarc/scif/ftdr-clears-tend",
                   test_ftdr_clears_tend);
    qtest_add_func("/rzv2l-smarc/scif/tend-deferred", test_tend_deferred);
    qtest_add_func("/rzv2l-smarc/scif/tend-te-cleared",
                   test_tend_te_cleared);
    qtest_add_func("/rzv2l-smarc/scif/rx-fifo", test_rx_fifo);
    qtest_add_func("/rzv2l-smarc/scif/rx-chardev", test_rx_chardev);
    qtest_add_func("/rzv2l-smarc/scif/rx-dr-timeout", test_rx_dr_timeout);
    qtest_add_func("/rzv2l-smarc/scif/rx-overrun", test_rx_overrun);
    qtest_add_func("/rzv2l-smarc/scif/rx-stopped-by-orer",
                   test_rx_stopped_by_orer);
    qtest_add_func("/rzv2l-smarc/scif/rx-disabled", test_rx_disabled);
    qtest_add_func("/rzv2l-smarc/scif/irq-txi", test_irq_txi);
    qtest_add_func("/rzv2l-smarc/scif/irq-rxi", test_irq_rxi);
    qtest_add_func("/rzv2l-smarc/scif/irq-tei-dri", test_irq_tei_dri);
    qtest_add_func("/rzv2l-smarc/scif/irq-bri", test_irq_bri);

    return g_test_run();
}
