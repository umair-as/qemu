/*
 * QTest testcase for the OS timer (OSTM) of the Renesas RZ/V2L
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Expected values come from the RZ/V2L Group User's Manual: Hardware
 * (R01UH0936EJ0150), section 20 (General Timer, GTM), never from the
 * model. The count clock is P0, 100 MHz on the board (CPG_PL2_DDIV at
 * its reset value 0): one count every 10 ns of virtual time.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define OSTM_BASE(n)        (0x12801000 + (n) * 0x400)   /* Table 20.2 */
#define CMP                 0x00
#define CNT                 0x04
#define TE                  0x10
#define TS                  0x14
#define TT                  0x18
#define CTL                 0x20
#define CTL_MD0             0x01
#define CTL_MD1             0x02

#define CLOCK_NS            10          /* P0 = 100 MHz */

/* Reset values (section 20.2.1, Table 20.4) */
#define CNT_RESET           0xffffffff
#define CNT_INIT_FREERUN    0x00000000

static uint32_t rd32(QTestState *qts, int n, uint32_t reg)
{
    return qtest_readl(qts, OSTM_BASE(n) + reg);
}

static uint8_t rd8(QTestState *qts, int n, uint32_t reg)
{
    return qtest_readb(qts, OSTM_BASE(n) + reg);
}

static void wr32(QTestState *qts, int n, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, OSTM_BASE(n) + reg, v);
}

static void wr8(QTestState *qts, int n, uint32_t reg, uint8_t v)
{
    qtest_writeb(qts, OSTM_BASE(n) + reg, v);
}

static void test_reset_values(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    int n;

    for (n = 0; n < 3; n++) {
        g_assert_cmphex(rd32(qts, n, CMP), ==, 0);
        g_assert_cmphex(rd32(qts, n, CNT), ==, CNT_RESET);
        g_assert_cmphex(rd8(qts, n, TE), ==, 0);
        g_assert_cmphex(rd8(qts, n, TS), ==, 0);
        g_assert_cmphex(rd8(qts, n, TT), ==, 0);
        g_assert_cmphex(rd8(qts, n, CTL), ==, 0);
    }
    qtest_quit(qts);
}

/*
 * Free-running comparison mode (20.3.6): CNT starts at 0 after the mode
 * change (Table 20.4), counts up once per count clock, keeps its value
 * when stopped, and restarts from 0 (20.2.2.3 Note 1). TS while running
 * is ignored (20.2.2.4).
 */
static void test_freerun(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    wr8(qts, 1, CTL, CTL_MD1);
    g_assert_cmphex(rd32(qts, 1, CNT), ==, CNT_INIT_FREERUN);
    wr8(qts, 1, TS, 1);
    g_assert_cmphex(rd8(qts, 1, TE), ==, 1);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(rd32(qts, 1, CNT), ==, 1000 / CLOCK_NS);
    qtest_clock_step(qts, 2340);
    g_assert_cmphex(rd32(qts, 1, CNT), ==, 3340 / CLOCK_NS);

    wr8(qts, 1, TS, 1);
    g_assert_cmphex(rd32(qts, 1, CNT), ==, 3340 / CLOCK_NS);

    wr8(qts, 1, TT, 1);
    g_assert_cmphex(rd8(qts, 1, TE), ==, 0);
    qtest_clock_step(qts, 5000);
    g_assert_cmphex(rd32(qts, 1, CNT), ==, 3340 / CLOCK_NS);

    wr8(qts, 1, TS, 1);
    qtest_clock_step(qts, 200);
    g_assert_cmphex(rd32(qts, 1, CNT), ==, 200 / CLOCK_NS);
    qtest_quit(qts);
}

/*
 * Interval timer mode (20.3.5): counts down from CMP and reloads it
 * every (CMP + 1) count clocks; a new CMP takes effect at the reload;
 * TS while running restarts from CMP (forced restart).
 */
static void test_interval(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    wr32(qts, 2, CMP, 999);
    wr8(qts, 2, TS, 1);
    g_assert_cmphex(rd32(qts, 2, CNT), ==, 999);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(rd32(qts, 2, CNT), ==, 999 - 100);

    /* A new CMP is loaded at the next reload, not at once */
    wr32(qts, 2, CMP, 499);
    g_assert_cmphex(rd32(qts, 2, CNT), ==, 999 - 100);
    qtest_clock_step(qts, 9000);    /* reload after 1000 clocks */
    g_assert_cmphex(rd32(qts, 2, CNT), ==, 499);
    qtest_clock_step(qts, 5000);    /* one period of 500 clocks */
    g_assert_cmphex(rd32(qts, 2, CNT), ==, 499);
    qtest_clock_step(qts, 300);
    g_assert_cmphex(rd32(qts, 2, CNT), ==, 499 - 30);

    /* Forced restart */
    wr8(qts, 2, TS, 1);
    g_assert_cmphex(rd32(qts, 2, CNT), ==, 499);
    qtest_quit(qts);
}

/*
 * CTL is writable only while the counter is stopped (20.2.2.6); a mode
 * change sets CNT to the mode's initial value (Table 20.4).
 */
static void test_ctl(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    wr8(qts, 0, CTL, CTL_MD1 | CTL_MD0);
    g_assert_cmphex(rd8(qts, 0, CTL), ==, CTL_MD1 | CTL_MD0);
    g_assert_cmphex(rd32(qts, 0, CNT), ==, CNT_INIT_FREERUN);
    wr8(qts, 0, CTL, 0);
    g_assert_cmphex(rd32(qts, 0, CNT), ==, CNT_RESET);

    wr8(qts, 0, CTL, 0xff);
    g_assert_cmphex(rd8(qts, 0, CTL), ==, CTL_MD1 | CTL_MD0);

    wr8(qts, 0, CTL, CTL_MD1);
    wr8(qts, 0, TS, 1);
    wr8(qts, 0, CTL, 0);
    g_assert_cmphex(rd8(qts, 0, CTL), ==, CTL_MD1);
    qtest_quit(qts);
}

/*
 * Interval mode with CMP = 0: the interrupt request stays asserted while
 * the counter runs and drops when it stops (20.3.5.2).
 */
static void test_interval_cmp0(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    qtest_irq_intercept_out_named(qts, "/machine/soc/ostm2", "sysbus-irq");
    g_assert_false(qtest_get_irq(qts, 0));
    wr8(qts, 2, TS, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1000);
    g_assert_true(qtest_get_irq(qts, 0));
    wr8(qts, 2, TT, 1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rzv2l-smarc/ostm/reset-values", test_reset_values);
    qtest_add_func("/rzv2l-smarc/ostm/freerun", test_freerun);
    qtest_add_func("/rzv2l-smarc/ostm/interval", test_interval);
    qtest_add_func("/rzv2l-smarc/ostm/ctl", test_ctl);
    qtest_add_func("/rzv2l-smarc/ostm/interval-cmp0", test_interval_cmp0);

    return g_test_run();
}
