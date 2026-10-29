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

    return g_test_run();
}
