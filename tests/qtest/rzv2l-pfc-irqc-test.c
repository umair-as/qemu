/*
 * QTest testcase for the pin function controller (PFC) and interrupt
 * control unit (IRQC) of the Renesas RZ/V2L
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Expected values come from the RZ/V2L Group User's Manual: Hardware
 * (R01UH0936EJ0150), sections 41 (PFC) and 8.5-8.6 (IRQC), never from
 * the model. The direct-boot values are registers read on the RZ/V2L
 * SMARC EVK at the U-Boot prompt.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define PFC                 0x11030000
#define P(off)              (PFC + 0x0000 + (off))
#define PM(off)             (PFC + 0x0100 + 2 * (off))
#define PMC(off)            (PFC + 0x0200 + (off))
#define PFCR(off)           (PFC + 0x0400 + 4 * (off))
#define PIN(off)            (PFC + 0x0800 + (off))
#define IOLH(off)           (PFC + 0x1000 + 8 * (off))
#define SR(off)             (PFC + 0x1400 + 8 * (off))
#define IEN(off)            (PFC + 0x1800 + 8 * (off))
#define SD_CH0              (PFC + 0x3000)
#define QSPI                (PFC + 0x3008)
#define PWPR                (PFC + 0x3014)
#define ETH_MODE            (PFC + 0x3018)
#define PWPR_B0WI           0x80
#define PWPR_PFCWE          0x40

/* Port index: off = 0x10 + port (section 41.2) */
#define OFF_P38             0x36        /* SCIF0, 2 pins */
#define OFF_P39             0x37        /* 3 pins */
#define OFF_P47             0x3f        /* SD0, 4 pins */
#define OFF_P19             0x23        /* SD1, 2 pins */
#define OFF_P48             0x40        /* SCIF2, 5 pins */

#define IRQC                0x110a0000
#define NSCR                0x00
#define NITSR               0x04
#define ISCR                0x10
#define IITSR               0x14
#define TSCR                0x20
#define TITSR0              0x24
#define TSSR(n)             (0x30 + 4 * (n))
#define BEISR1              0x54
#define EREISR0             0x60

/* PFC reset values (sections 41.2-41.3) */
static void test_pfc_reset(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    g_assert_cmphex(qtest_readb(qts, PWPR), ==, PWPR_B0WI);       /* p.3379 */
    g_assert_cmphex(qtest_readb(qts, ETH_MODE), ==, 0x03);        /* p.3384 */
    g_assert_cmphex(qtest_readb(qts, SD_CH0), ==, 0);
    g_assert_cmphex(qtest_readb(qts, PMC(OFF_P38)), ==, 0);
    g_assert_cmphex(qtest_readw(qts, PM(OFF_P38)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PFCR(OFF_P38)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, IEN(4)), ==, 0);
    /*
     * IOLH: drive ability resets to 01b for pin group A and 10b for
     * group B (section 41.3.7 Note 1, Table 41.7): IOLH10 holds two
     * group A pins (P0), IOLH06 three group B pins (SD0).
     */
    g_assert_cmphex(qtest_readl(qts, IOLH(0x10)), ==, 0x00000101);
    g_assert_cmphex(qtest_readl(qts, IOLH(0x06)), ==, 0x00020202);
    /* SR resets to 1 per pin (section 41.3.8) */
    g_assert_cmphex(qtest_readl(qts, SR(0x06)), ==, 0x00010101);
    qtest_quit(qts);
}

/*
 * Post-firmware values read on the EVK (rows R30-R36): TF-A sets the
 * SCIF0, SD0 and SD1 pin functions; SD0 is at 1.8 V; the Ethernet TXC
 * pins are outputs.
 */
static void test_pfc_direct_boot(void)
{
    g_autofree char *kernel = NULL;
    int fd = g_file_open_tmp("rzv2l-pfc-XXXXXX", &kernel, NULL);
    QTestState *qts;

    g_assert(fd >= 0);
    close(fd);
    qts = qtest_initf("-machine rzv2l-smarc -kernel %s", kernel);
    g_assert_cmphex(qtest_readb(qts, PMC(OFF_P38)), ==, 0x03);
    g_assert_cmphex(qtest_readl(qts, PFCR(OFF_P38)), ==, 0x00000011);
    g_assert_cmphex(qtest_readw(qts, PM(OFF_P38)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, PMC(OFF_P47)), ==, 0x03);
    g_assert_cmphex(qtest_readl(qts, PFCR(OFF_P47)), ==, 0x00000011);
    g_assert_cmphex(qtest_readb(qts, PMC(OFF_P19)), ==, 0x03);
    g_assert_cmphex(qtest_readl(qts, PFCR(OFF_P19)), ==, 0x00000011);
    g_assert_cmphex(qtest_readb(qts, SD_CH0), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, QSPI), ==, 0);
    g_assert_cmphex(qtest_readb(qts, ETH_MODE), ==, 0);
    g_assert_cmphex(qtest_readb(qts, PWPR), ==, PWPR_B0WI);
    g_assert_cmphex(qtest_readl(qts, IEN(4)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, PMC(OFF_P48)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PFCR(OFF_P48)), ==, 0);
    qtest_quit(qts);
    unlink(kernel);
}

/*
 * PWPR (section 41.3.10): PFC registers take writes only while PFCWE is
 * 1; PFCWE changes only while B0WI is 0. PMC is not protected. The
 * sequence is the one Linux and TF-A use, with 32-bit PWPR stores.
 */
static void test_pfc_pwpr(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    /* Locked: the PFC write is dropped, the PMC write is not */
    qtest_writel(qts, PFCR(OFF_P48), 0x00011111);
    g_assert_cmphex(qtest_readl(qts, PFCR(OFF_P48)), ==, 0);
    qtest_writeb(qts, PMC(OFF_P48), 0x1f);
    g_assert_cmphex(qtest_readb(qts, PMC(OFF_P48)), ==, 0x1f);

    /* PFCWE cannot be set while B0WI is 1 */
    qtest_writel(qts, PWPR, PWPR_B0WI | PWPR_PFCWE);
    g_assert_cmphex(qtest_readb(qts, PWPR), ==, PWPR_B0WI);

    /* Unlock: B0WI = 0, then PFCWE = 1 */
    qtest_writel(qts, PWPR, 0);
    qtest_writel(qts, PWPR, PWPR_PFCWE);
    g_assert_cmphex(qtest_readb(qts, PWPR), ==, PWPR_PFCWE);
    /* P48 has 5 pins; bit 3 of each nibble is not implemented */
    qtest_writel(qts, PFCR(OFF_P48), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, PFCR(OFF_P48)), ==, 0x00077777);

    /* Relock: PFCWE = 0, then B0WI = 1 */
    qtest_writel(qts, PWPR, 0);
    qtest_writel(qts, PWPR, PWPR_B0WI);
    qtest_writel(qts, PFCR(OFF_P48), 0);
    g_assert_cmphex(qtest_readl(qts, PFCR(OFF_P48)), ==, 0x00077777);
    qtest_quit(qts);
}

/*
 * Port data and mode (sections 41.3.1-41.3.4, Table 41.1): a 3-pin port
 * keeps only its pins' bits; PM is 2 bits per pin. PIN reflects Pn for
 * pins in output mode with input enabled (PMn = 11b): the model's
 * assumption that no pin is driven from outside, not a manual value
 * (section 41.3.5 says only "input value of the pin").
 */
static void test_pfc_port(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    qtest_writew(qts, PM(OFF_P39), 0xffff);
    g_assert_cmphex(qtest_readw(qts, PM(OFF_P39)), ==, 0x003f);
    qtest_writeb(qts, P(OFF_P39), 0xff);
    g_assert_cmphex(qtest_readb(qts, P(OFF_P39)), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, PIN(OFF_P39)), ==, 0x07);

    /* Pin 1 output only (10b), pins 0 and 2 output + input (11b) */
    qtest_writew(qts, PM(OFF_P39), 0x0033 | (0x2 << 2));
    g_assert_cmphex(qtest_readb(qts, PIN(OFF_P39)), ==, 0x05);
    qtest_writeb(qts, P(OFF_P39), 0x02);
    g_assert_cmphex(qtest_readb(qts, PIN(OFF_P39)), ==, 0x00);

    /* PIN is read-only */
    qtest_writeb(qts, PIN(OFF_P39), 0xff);
    g_assert_cmphex(qtest_readb(qts, PIN(OFF_P39)), ==, 0x00);

    /* IEN04: the two AUDIO_CLK pins, bits 0 and 8 (section 41.3.20) */
    qtest_writel(qts, IEN(4), 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, IEN(4)), ==, 0x00000101);

    /*
     * An offset with no register reads 0 (EVK R71: +0x3100); that the
     * write is ignored is an assumption (R71 was a read only).
     */
    qtest_writel(qts, PFC + 0x3100, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, PFC + 0x3100), ==, 0);
    qtest_quit(qts);
}

/* IRQC (Table 8.3 p.549, section 8.6) */
static void test_irqc(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    int n;

    for (n = 0; n <= 0x64; n += 4) {
        if ((n >= 0x08 && n < 0x10) || (n >= 0x18 && n < 0x20) ||
            n == 0x2c || (n >= 0x58 && n < 0x60)) {
            continue;
        }
        g_assert_cmphex(qtest_readl(qts, IRQC + n), ==, 0);
    }

    /* Selection registers keep their implemented bits */
    qtest_writel(qts, IRQC + NITSR, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, IRQC + NITSR), ==, 0x00000001);
    qtest_writel(qts, IRQC + IITSR, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, IRQC + IITSR), ==, 0x0000ffff);
    qtest_writel(qts, IRQC + TITSR0, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, IRQC + TITSR0), ==, 0xa5a5a5a5);
    qtest_writel(qts, IRQC + TSSR(7), 0x81828384);
    g_assert_cmphex(qtest_readl(qts, IRQC + TSSR(7)), ==, 0x81828384);

    /* Status bits can only be cleared: writing 1 is invalid */
    qtest_writel(qts, IRQC + NSCR, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, IRQC + NSCR), ==, 0);
    qtest_writel(qts, IRQC + ISCR, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, IRQC + ISCR), ==, 0);
    qtest_writel(qts, IRQC + TSCR, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, IRQC + TSCR), ==, 0);
    qtest_writel(qts, IRQC + BEISR1, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, IRQC + BEISR1), ==, 0);
    qtest_writel(qts, IRQC + EREISR0, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, IRQC + EREISR0), ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, IRQC + IITSR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, IRQC + TSSR(7)), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rzv2l-smarc/pfc/reset-values", test_pfc_reset);
    qtest_add_func("/rzv2l-smarc/pfc/direct-boot", test_pfc_direct_boot);
    qtest_add_func("/rzv2l-smarc/pfc/pwpr", test_pfc_pwpr);
    qtest_add_func("/rzv2l-smarc/pfc/port", test_pfc_port);
    qtest_add_func("/rzv2l-smarc/irqc/registers", test_irqc);

    return g_test_run();
}
