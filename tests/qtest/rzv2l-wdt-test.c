/*
 * QTest testcase for the watchdog timer (WDT) of the Renesas RZ/V2L
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Expected values come from the RZ/V2L Group User's Manual: Hardware
 * (R01UH0936EJ0150), section 21 (WDT) and the CPG watchdog registers of
 * section 7.2.4.182-183, never from the model. The counter clock
 * WDTn_CLK is 24 MHz (section 21.1.1). Without a kernel the machine
 * starts at the manual reset values.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* WDT0-2 (Table 21.3) */
static const uint32_t wdt_base[] = { 0x12800800, 0x12800c00, 0x12800400 };
#define WDTCNT              0x00
#define WDTSET              0x04
#define WDTTIM              0x08
#define WDTINT              0x0c
#define PECR                0x10
#define PEEN                0x14
#define PESR                0x18
#define PEER                0x1c
#define PEPO                0x20

#define CPG_BASE            0x11010000
#define CPG_RST_WDT         0x848
#define CPG_WDTOVF_RST      0xb10
#define CPG_WDTRST_SEL      0xb14

/* Table 21.4 and sections 7.2.4.121, 7.2.4.182-183 */
#define WDTSET_RESET        0xfff00000
#define RST_WDT_RESET       0x00000009
#define WDTRST_SEL_RESET    0x00000088

/* WDTTIME = 0: one cycle is 1024 x 1024 clocks at 24 MHz (section 21.3.2) */
#define CYCLE_CLOCKS        0x100000ull
#define CYCLE_NS            (CYCLE_CLOCKS * 1000000000ull / 24000000)

static uint32_t wdt_rd(QTestState *qts, int n, uint32_t reg)
{
    return qtest_readl(qts, wdt_base[n] + reg);
}

static void wdt_wr(QTestState *qts, int n, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, wdt_base[n] + reg, v);
}

static uint32_t cpg_rd(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, CPG_BASE + reg);
}

static void cpg_wr(QTestState *qts, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, CPG_BASE + reg, v);
}

/* Start WDT0 with WDTTIME 0 from a count of 0 */
static void wdt0_start_shortest(QTestState *qts)
{
    wdt_wr(qts, 0, WDTSET, 0);
    wdt_wr(qts, 0, WDTTIM, 0);
    wdt_wr(qts, 0, WDTCNT, 1);
}

static void test_reset_values(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    g_assert_cmphex(wdt_rd(qts, 0, WDTCNT), ==, 0);
    g_assert_cmphex(wdt_rd(qts, 0, WDTSET), ==, WDTSET_RESET);
    g_assert_cmphex(wdt_rd(qts, 0, WDTTIM), ==, 0);
    g_assert_cmphex(wdt_rd(qts, 0, WDTINT), ==, 0);
    g_assert_cmphex(wdt_rd(qts, 0, PECR), ==, 0);
    g_assert_cmphex(wdt_rd(qts, 0, PEEN), ==, 0);
    g_assert_cmphex(wdt_rd(qts, 0, PESR), ==, 0);
    g_assert_cmphex(wdt_rd(qts, 0, PEER), ==, 0);
    g_assert_cmphex(wdt_rd(qts, 0, PEPO), ==, 0);
    g_assert_cmphex(cpg_rd(qts, CPG_RST_WDT), ==, RST_WDT_RESET);
    g_assert_cmphex(cpg_rd(qts, CPG_WDTRST_SEL), ==, WDTRST_SEL_RESET);
    g_assert_cmphex(cpg_rd(qts, CPG_WDTOVF_RST), ==, 0);

    /* WDTSET: only WDTTIME, bits 31:20, is writable */
    wdt_wr(qts, 0, WDTSET, 0xffffffff);
    g_assert_cmphex(wdt_rd(qts, 0, WDTSET), ==, 0xfff00000);
    wdt_wr(qts, 0, WDTSET, 0x2ae00000);
    g_assert_cmphex(wdt_rd(qts, 0, WDTSET), ==, 0x2ae00000);
    /* PEEN: bit 0 only; the other parity registers keep all 32 bits */
    wdt_wr(qts, 0, PEEN, 0xfffffffe);
    g_assert_cmphex(wdt_rd(qts, 0, PEEN), ==, 0);
    wdt_wr(qts, 0, PEPO, 0xa5a5a5a5);
    g_assert_cmphex(wdt_rd(qts, 0, PEPO), ==, 0xa5a5a5a5);
    qtest_quit(qts);
}

static void test_count(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    wdt0_start_shortest(qts);
    g_assert_cmphex(wdt_rd(qts, 0, WDTCNT), ==, 1);
    qtest_clock_step(qts, 1000000);     /* 1 ms: 24000 clocks */
    g_assert_cmpuint(wdt_rd(qts, 0, WDTTIM), ==, 24000);

    /* Running: WDTSET and WDTTIM keep their values, WDTEN stays set */
    wdt_wr(qts, 0, WDTSET, 0x2ae00000);
    wdt_wr(qts, 0, WDTTIM, 0x1234);
    wdt_wr(qts, 0, WDTCNT, 0);
    g_assert_cmphex(wdt_rd(qts, 0, WDTSET), ==, 0);
    g_assert_cmpuint(wdt_rd(qts, 0, WDTTIM), ==, 24000);
    g_assert_cmphex(wdt_rd(qts, 0, WDTCNT), ==, 1);
    qtest_quit(qts);
}

static void test_first_overflow(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    qtest_irq_intercept_out_named(qts, "/machine/soc/wdt0", "sysbus-irq");
    wdt0_start_shortest(qts);

    /* Section 21.4.1: the interrupt comes at the end of the first cycle */
    qtest_clock_step(qts, CYCLE_NS - 1000);
    g_assert_cmphex(wdt_rd(qts, 0, WDTINT), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(wdt_rd(qts, 0, WDTINT), ==, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    /* The counter restarted from 0 at the overflow */
    g_assert_cmpuint(wdt_rd(qts, 0, WDTTIM), <, 48);

    /* Writing INTDISP = 1 clears the interrupt and the counter */
    qtest_clock_step(qts, 1000000);
    wdt_wr(qts, 0, WDTINT, 1);
    g_assert_cmphex(wdt_rd(qts, 0, WDTINT), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmpuint(wdt_rd(qts, 0, WDTTIM), ==, 0);
    qtest_quit(qts);
}

static void test_reset_request(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    /* Masked at reset (WDTRST_SEL 0x88): the second overflow resets nothing */
    wdt0_start_shortest(qts);
    qtest_clock_step(qts, 2 * CYCLE_NS + 1000);
    g_assert_cmphex(wdt_rd(qts, 0, WDTCNT), ==, 1);
    g_assert_cmphex(cpg_rd(qts, CPG_WDTOVF_RST), ==, 0);
    qtest_quit(qts);

    /* WDTRSTSEL0 set: the second overflow resets the system */
    qts = qtest_init("-machine rzv2l-smarc");
    cpg_wr(qts, CPG_WDTRST_SEL, WDTRST_SEL_RESET | 0x1);
    wdt0_start_shortest(qts);
    qtest_clock_step(qts, 2 * CYCLE_NS - 1000);
    g_assert_cmphex(wdt_rd(qts, 0, WDTCNT), ==, 1);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(wdt_rd(qts, 0, WDTCNT), ==, 0);
    g_assert_cmphex(wdt_rd(qts, 0, WDTSET), ==, WDTSET_RESET);
    /* The flag survives the reset it reports; WDTRST_SEL does not */
    g_assert_cmphex(cpg_rd(qts, CPG_WDTOVF_RST), ==, 0x1);
    g_assert_cmphex(cpg_rd(qts, CPG_WDTRST_SEL), ==, WDTRST_SEL_RESET);

    /* Cleared by writing 1, under its write-enable bit */
    cpg_wr(qts, CPG_WDTOVF_RST, 0x1);
    g_assert_cmphex(cpg_rd(qts, CPG_WDTOVF_RST), ==, 0x1);
    cpg_wr(qts, CPG_WDTOVF_RST, 0x10001);
    g_assert_cmphex(cpg_rd(qts, CPG_WDTOVF_RST), ==, 0);
    qtest_quit(qts);
}

static void test_refresh(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    int i;

    cpg_wr(qts, CPG_WDTRST_SEL, WDTRST_SEL_RESET | 0x1);
    wdt0_start_shortest(qts);
    /* Cleared every 0.9 cycle: no interrupt, no reset for 5 cycles */
    for (i = 0; i < 6; i++) {
        qtest_clock_step(qts, CYCLE_NS * 9 / 10);
        g_assert_cmphex(wdt_rd(qts, 0, WDTINT), ==, 0);
        wdt_wr(qts, 0, WDTINT, 1);
    }
    g_assert_cmphex(wdt_rd(qts, 0, WDTCNT), ==, 1);
    g_assert_cmphex(cpg_rd(qts, CPG_WDTOVF_RST), ==, 0);
    qtest_quit(qts);
}

static void test_parity_force(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    /* Table 21.5 row 8: PECR = 1, PEEN = 1 asserts PERROUT, no reset */
    qtest_irq_intercept_out_named(qts, "/machine/soc/wdt0", "sysbus-irq");
    cpg_wr(qts, CPG_WDTRST_SEL, WDTRST_SEL_RESET | 0x1);
    wdt_wr(qts, 0, PECR, 1);
    wdt_wr(qts, 0, PEEN, 1);
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_cmphex(cpg_rd(qts, CPG_WDTOVF_RST), ==, 0);
    wdt_wr(qts, 0, PEEN, 0);
    g_assert_false(qtest_get_irq(qts, 1));

    /* Row 7: PECR = 0, PEEN = 1 asserts WDTRSTB, as Linux restarts */
    wdt_wr(qts, 0, PECR, 0);
    wdt_wr(qts, 0, PEEN, 1);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(cpg_rd(qts, CPG_WDTOVF_RST), ==, 0x1);
    g_assert_cmphex(wdt_rd(qts, 0, PEEN), ==, 0);
    qtest_quit(qts);
}

static void test_presetn(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    /* RST_WDT 0x9 holds WDT1 in reset: its registers ignore writes */
    wdt_wr(qts, 1, WDTSET, 0);
    g_assert_cmphex(wdt_rd(qts, 1, WDTSET), ==, WDTSET_RESET);
    cpg_wr(qts, CPG_RST_WDT, 0x00020002);
    wdt_wr(qts, 1, WDTSET, 0);
    g_assert_cmphex(wdt_rd(qts, 1, WDTSET), ==, 0);

    /* Applying the reset stops a running channel, as Linux stops it */
    wdt0_start_shortest(qts);
    cpg_wr(qts, CPG_RST_WDT, 0x00010000);
    g_assert_cmphex(wdt_rd(qts, 0, WDTCNT), ==, 0);
    g_assert_cmphex(wdt_rd(qts, 0, WDTSET), ==, WDTSET_RESET);
    cpg_wr(qts, CPG_RST_WDT, 0x00010001);
    qtest_clock_step(qts, 2 * CYCLE_NS + 1000);
    g_assert_cmphex(wdt_rd(qts, 0, WDTCNT), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rzv2l-wdt/reset-values", test_reset_values);
    qtest_add_func("/rzv2l-wdt/count", test_count);
    qtest_add_func("/rzv2l-wdt/first-overflow", test_first_overflow);
    qtest_add_func("/rzv2l-wdt/reset-request", test_reset_request);
    qtest_add_func("/rzv2l-wdt/refresh", test_refresh);
    qtest_add_func("/rzv2l-wdt/parity-force", test_parity_force);
    qtest_add_func("/rzv2l-wdt/presetn", test_presetn);
    return g_test_run();
}
