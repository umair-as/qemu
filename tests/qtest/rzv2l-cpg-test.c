/*
 * QTest testcase for the Clock Pulse Generator of the Renesas RZ/V2L
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Expected values come from the RZ/V2L Group User's Manual: Hardware
 * (R01UH0936EJ0150), Table 7.5 (p.246-251) and the register
 * descriptions of section 7.2.4, never from the model. The direct-boot
 * values are the registers read on the RZ/V2L SMARC EVK at the U-Boot
 * prompt, after TF-A and U-Boot set up the clocks.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define CPG_BASE            0x11010000

#define SAMPLL1_CLK1        0x004
#define SAMPLL1_CLK2        0x008
#define PL1_DDIV            0x200
#define PL2_DDIV            0x204
#define PL3A_DDIV           0x208
#define PL3C_SDIV           0x214
#define PL2SDHI_DSEL        0x218
#define PL4_DSEL            0x21c
#define CLKSTATUS           0x280
#define PL3_SSEL            0x408
#define PL6_SSEL            0x414
#define PL6_ETH_SSEL        0x418
#define CLKON_CA55          0x500
#define CLKON_GIC600        0x514
#define CLKON_IA55          0x518
#define CLKON_SYC           0x528
#define CLKON_GTM           0x534
#define CLKON_WDT           0x548
#define CLKON_DDR           0x54c
#define CLKON_SPI_MULTI     0x550
#define CLKON_SDHI          0x554
#define CLKON_USB           0x578
#define CLKON_SCIF          0x584
#define CLKON_GPIO          0x598
#define CLKON_MBUS          0x5b8
#define CLKON_DRP           0x5e8
#define RST_CA55            0x800
#define RST_GIC600          0x814
#define RST_SYC             0x828
#define RST_GTM             0x834
#define RST_WDT             0x848
#define RST_DDR             0x84c
#define RST_SPI             0x850
#define RST_SDHI            0x854
#define RST_USB             0x878
#define RST_ETH             0x87c
#define RST_I2C             0x880
#define RST_SCIF            0x884
#define RST_GPIO            0x898
#define RST_DRP             0x8e8
#define MON                 0x180   /* CLK_MON = CLK_ON + 0x180, likewise RST */
#define MSTOP_MCPU1         0xb64
#define MSTOP_MCPU2         0xb68
#define MSTOP_REG0          0xb7c
#define OTHERFUNC1          0xbe8

#define WEN(bits)           ((uint32_t)(bits) << 16)

typedef struct {
    uint32_t offset;
    uint32_t value;
} RegValue;

/* Reset values (Table 7.5 unless stated) */
static const RegValue reset_values[] = {
    /* PLL1 = 24 MHz x 100 / 2 = 1200 MHz (7.2.4.2-3, p.255-259) */
    { SAMPLL1_CLK1, 0x00001901 },
    { SAMPLL1_CLK2, 0x00180601 },
    /* Table 7.5 writes H'0000 000x; the bit table (7.2.4.16 p.274) 00b */
    { PL1_DDIV, 0 },
    { PL2_DDIV, 0 },
    { PL3A_DDIV, 0 },
    { PL3C_SDIV, 0x00000401 },
    { PL2SDHI_DSEL, 0x00000011 },
    { PL4_DSEL, 0 },
    { CLKSTATUS, 0 },
    { PL3_SSEL, 0x00000111 },
    { PL6_SSEL, 0x00000101 },
    { PL6_ETH_SSEL, 0x00000010 },
    { CLKON_CA55, 0x3f },        { CLKON_CA55 + MON, 0x3f },
    { CLKON_GIC600, 0x1 },       { CLKON_GIC600 + MON, 0x1 },
    { CLKON_IA55, 0x3 },         { CLKON_IA55 + MON, 0x3 },
    { CLKON_GTM, 0x1 },          { CLKON_GTM + MON, 0x1 },
    { CLKON_SDHI, 0 },           { CLKON_SDHI + MON, 0 },
    { CLKON_SCIF, 0 },           { CLKON_SCIF + MON, 0 },
    { CLKON_GPIO, 0 },           { CLKON_GPIO + MON, 0 },
    { CLKON_MBUS, 0x7ff },       { CLKON_MBUS + MON, 0x7ff },
    { CLKON_DRP, 0 },            { CLKON_DRP + MON, 0 },
    /*
     * CPG_CLKON_WDT bits 7:6 are reserved and read 1 (7.2.4.44 p.308).
     * Table 7.5 gives CPG_CLKMON_WDT the same 0xc3, but the EVK reads
     * those two bits as 0 in the monitor (CLK_ON 0xff, CLK_MON 0x3f):
     * silicon decides, so the monitor reads 0x03 here.
     */
    { CLKON_WDT, 0xc3 },         { CLKON_WDT + MON, 0x03 },
    { RST_CA55, 0x1fff },        { RST_CA55 + MON, 0 },
    { RST_GIC600, 0x3 },         { RST_GIC600 + MON, 0 },
    { RST_GTM, 0x1 },            { RST_GTM + MON, 0x6 },
    /* bit 3 of CPG_RST_WDT is reserved and reads 1 (7.2.4.121 p.394) */
    { RST_WDT, 0x9 },            { RST_WDT + MON, 0x6 },
    { RST_DDR, 0 },              { RST_DDR + MON, 0x7f },
    { RST_SDHI, 0 },             { RST_SDHI + MON, 0x3 },
    { RST_SCIF, 0 },             { RST_SCIF + MON, 0x1f },
    { RST_GPIO, 0 },             { RST_GPIO + MON, 0x7 },
    { RST_DRP, 0 },              { RST_DRP + MON, 0x1 },
    { MSTOP_MCPU2, 0 },
    { MSTOP_REG0, 0 },
    { OTHERFUNC1, 0 },
};

/*
 * Post-firmware values read on the EVK: every register the firmware
 * leaves at a value other than its reset value, with its monitor
 * (CPG rows R10-R19, R72 and the full CLK_ON/CLK_MON and RST/RSTMON
 * dumps X01, X02).
 */
static const RegValue post_firmware_values[] = {
    { SAMPLL1_CLK1, 0x00001901 },
    { SAMPLL1_CLK2, 0x00180601 },
    { PL1_DDIV, 0 },
    { PL3A_DDIV, 0x00000100 },
    { PL3C_SDIV, 0x00000401 },
    { PL2SDHI_DSEL, 0x00000022 },
    { PL4_DSEL, 0x00000001 },
    { PL3_SSEL, 0x00000011 },
    { CLKON_GIC600, 0x1 },       { CLKON_GIC600 + MON, 0x1 },
    { CLKON_IA55, 0x3 },         { CLKON_IA55 + MON, 0x3 },
    { CLKON_SYC, 0x1 },          { CLKON_SYC + MON, 0x1 },
    { CLKON_GTM, 0 },            { CLKON_GTM + MON, 0 },
    { CLKON_DDR, 0x3 },          { CLKON_DDR + MON, 0x3 },
    { CLKON_SPI_MULTI, 0x3 },    { CLKON_SPI_MULTI + MON, 0x3 },
    { CLKON_USB, 0xf },          { CLKON_USB + MON, 0xf },
    { CLKON_WDT, 0xff },         { CLKON_WDT + MON, 0x3f },
    { CLKON_SDHI, 0xff },        { CLKON_SDHI + MON, 0xff },
    { CLKON_SCIF, 0x1 },         { CLKON_SCIF + MON, 0x1 },
    { CLKON_GPIO, 0x1 },         { CLKON_GPIO + MON, 0x1 },
    { RST_GIC600, 0x3 },         { RST_GIC600 + MON, 0 },
    { RST_SYC, 0x1 },            { RST_SYC + MON, 0 },
    { RST_GTM, 0x1 },            { RST_GTM + MON, 0x6 },
    { RST_WDT, 0xf },            { RST_WDT + MON, 0 },
    { RST_DDR, 0x7f },           { RST_DDR + MON, 0 },
    { RST_SPI, 0x1 },            { RST_SPI + MON, 0 },
    { RST_SDHI, 0x3 },           { RST_SDHI + MON, 0 },
    { RST_USB, 0xf },            { RST_USB + MON, 0 },
    { RST_ETH, 0x3 },            { RST_ETH + MON, 0 },
    { RST_I2C, 0xf },            { RST_I2C + MON, 0 },
    { RST_SCIF, 0x1 },           { RST_SCIF + MON, 0x1e },
    { RST_GPIO, 0x7 },           { RST_GPIO + MON, 0 },
    { RST_DRP, 0 },              { RST_DRP + MON, 0x1 },
    { MSTOP_MCPU2, 0 },
};

static uint32_t cpg_read(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, CPG_BASE + offset);
}

static void cpg_write(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, CPG_BASE + offset, value);
}

static void check_values(QTestState *qts, const RegValue *v, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t got = cpg_read(qts, v[i].offset);

        if (got != v[i].value) {
            g_test_message("offset 0x%03x", v[i].offset);
        }
        g_assert_cmphex(got, ==, v[i].value);
    }
}

static void test_reset_values(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    check_values(qts, reset_values, ARRAY_SIZE(reset_values));
    qtest_quit(qts);
}

/* With -kernel and no firmware, the board starts the CPG post-firmware */
static void test_direct_boot(void)
{
    g_autofree char *kernel = NULL;
    int fd = g_file_open_tmp("rzv2l-cpg-XXXXXX", &kernel, NULL);
    QTestState *qts;

    g_assert(fd >= 0);
    close(fd);
    qts = qtest_initf("-machine rzv2l-smarc -kernel %s", kernel);
    check_values(qts, post_firmware_values, ARRAY_SIZE(post_firmware_values));

    /* A system reset brings back the post-firmware state, not Table 7.5 */
    cpg_write(qts, CLKON_GTM, WEN(0x7) | 0x6);
    cpg_write(qts, RST_SCIF, WEN(0x1));
    qtest_system_reset(qts);
    check_values(qts, post_firmware_values, ARRAY_SIZE(post_firmware_values));
    qtest_quit(qts);
    unlink(kernel);
}

/*
 * A field changes only with its write-enable bit, 16 above the field's
 * lowest bit; the enable bits read 0 (7.2.4.17, 7.2.4.18, 7.2.4.58).
 */
static void test_write_enable(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    /* CPG_CLKON_SCIF: one enable per bit */
    cpg_write(qts, CLKON_SCIF, 0x1f);
    g_assert_cmphex(cpg_read(qts, CLKON_SCIF), ==, 0);
    cpg_write(qts, CLKON_SCIF, WEN(0x5) | 0x1f);
    g_assert_cmphex(cpg_read(qts, CLKON_SCIF), ==, 0x5);
    cpg_write(qts, CLKON_SCIF, WEN(0x1));
    g_assert_cmphex(cpg_read(qts, CLKON_SCIF), ==, 0x4);

    /*
     * CPG_PL2_DDIV: bit 16 enables DIVPL2A_SET[2:0], bit 28 enables
     * DIVDSILPCLK_SET[13:12]. A hiword write as Linux does it also sets
     * bits 17-18, which have no function.
     */
    cpg_write(qts, PL2_DDIV, WEN(0x7) | 0x3005);
    g_assert_cmphex(cpg_read(qts, PL2_DDIV), ==, 0x0005);
    cpg_write(qts, PL2_DDIV, WEN(0x6) | 0x3002);
    g_assert_cmphex(cpg_read(qts, PL2_DDIV), ==, 0x0005);
    cpg_write(qts, PL2_DDIV, WEN(0x3000) | 0x2007);
    g_assert_cmphex(cpg_read(qts, PL2_DDIV), ==, 0x2005);

    /* CPG_PL3A_DDIV: three 3-bit fields, enables at 16, 20, 24 */
    cpg_write(qts, PL3A_DDIV, WEN(0x070) | 0x0320);
    g_assert_cmphex(cpg_read(qts, PL3A_DDIV), ==, 0x0020);
    cpg_write(qts, PL3A_DDIV, WEN(0x700) | 0x0700);
    g_assert_cmphex(cpg_read(qts, PL3A_DDIV), ==, 0x0720);

    /* SEL_SDHI0_SET[1:0] (bit 16) and SEL_SDHI1_SET[5:4] (bit 20) */
    cpg_write(qts, PL2SDHI_DSEL, WEN(0x30) | 0x33);
    g_assert_cmphex(cpg_read(qts, PL2SDHI_DSEL), ==, 0x31);

    /* Reserved bits keep their reset value whatever is written */
    cpg_write(qts, CLKON_WDT, WEN(0xffff));
    g_assert_cmphex(cpg_read(qts, CLKON_WDT), ==, 0xc0);
    cpg_write(qts, PL3_SSEL, WEN(0xffff));
    g_assert_cmphex(cpg_read(qts, PL3_SSEL), ==, 0x011);
    cpg_write(qts, PL3C_SDIV, WEN(0xffff));
    g_assert_cmphex(cpg_read(qts, PL3C_SDIV), ==, 0x001);

    /*
     * MSTOP: one enable per bit. Bit 3 of CPG_BUS_MCPU1_MSTOP is
     * reserved and reads 0 (7.2.4.191).
     */
    cpg_write(qts, MSTOP_MCPU1, WEN(0xffff) | 0xffff);
    g_assert_cmphex(cpg_read(qts, MSTOP_MCPU1), ==, 0xfff7);
    cpg_write(qts, MSTOP_MCPU1, WEN(0x0001));
    g_assert_cmphex(cpg_read(qts, MSTOP_MCPU1), ==, 0xfff6);
    cpg_write(qts, MSTOP_MCPU2, 0x0002);
    g_assert_cmphex(cpg_read(qts, MSTOP_MCPU2), ==, 0);
    cpg_write(qts, MSTOP_MCPU2, WEN(0x0002) | 0x0002);
    g_assert_cmphex(cpg_read(qts, MSTOP_MCPU2), ==, 0x0002);

    /* CPG_SAMPLL1_CLK1 has no enables: DIV_K is bits 31:16 */
    cpg_write(qts, SAMPLL1_CLK1, 0xfffe1a42);
    g_assert_cmphex(cpg_read(qts, SAMPLL1_CLK1), ==, 0xfffe1a42);

    qtest_quit(qts);
}

/*
 * A clock monitor bit is 1 while the clock is supplied, like CLK_ON
 * (7.2.4.97 p.368); a reset monitor bit is 1 while the reset is
 * applied, the inverse of RST (7.2.4.135 p.409, 7.2.4.173 p.450). Linux
 * polls each right after its write, so the monitor follows at once.
 */
static void test_monitors(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    cpg_write(qts, CLKON_SCIF, WEN(0x1) | 0x1);
    g_assert_cmphex(cpg_read(qts, CLKON_SCIF + MON), ==, 0x01);
    cpg_write(qts, CLKON_SCIF, WEN(0x12) | 0x10);
    g_assert_cmphex(cpg_read(qts, CLKON_SCIF + MON), ==, 0x11);
    cpg_write(qts, CLKON_SCIF, WEN(0x1));
    g_assert_cmphex(cpg_read(qts, CLKON_SCIF + MON), ==, 0x10);

    /* CLKMON_WDT follows bits 5:0 only; MBUS keeps its reserved 1s */
    cpg_write(qts, CLKON_WDT, WEN(0x3f) | 0x24);
    g_assert_cmphex(cpg_read(qts, CLKON_WDT), ==, 0xe4);
    g_assert_cmphex(cpg_read(qts, CLKON_WDT + MON), ==, 0x24);
    cpg_write(qts, CLKON_MBUS, WEN(0x208));
    g_assert_cmphex(cpg_read(qts, CLKON_MBUS), ==, 0x5f7);
    g_assert_cmphex(cpg_read(qts, CLKON_MBUS + MON), ==, 0x5f7);

    /* Release SCIF0, then SCIF0 and SCIF3; apply the reset again */
    cpg_write(qts, RST_SCIF, WEN(0x1) | 0x1);
    g_assert_cmphex(cpg_read(qts, RST_SCIF + MON), ==, 0x1e);
    cpg_write(qts, RST_SCIF, WEN(0x8) | 0x8);
    g_assert_cmphex(cpg_read(qts, RST_SCIF + MON), ==, 0x16);
    cpg_write(qts, RST_SCIF, WEN(0x1));
    g_assert_cmphex(cpg_read(qts, RST_SCIF + MON), ==, 0x17);

    /* RST_WDT bit 3 reads 1 but is no reset: the monitor has 3 bits */
    cpg_write(qts, RST_WDT, WEN(0x7) | 0x2);
    g_assert_cmphex(cpg_read(qts, RST_WDT), ==, 0xa);
    g_assert_cmphex(cpg_read(qts, RST_WDT + MON), ==, 0x5);

    /* The monitors and CPG_CLKSTATUS are read-only */
    cpg_write(qts, RST_SCIF + MON, 0xffffffff);
    g_assert_cmphex(cpg_read(qts, RST_SCIF + MON), ==, 0x17);
    cpg_write(qts, CLKON_SCIF + MON, 0xffffffff);
    g_assert_cmphex(cpg_read(qts, CLKON_SCIF + MON), ==, 0x10);
    cpg_write(qts, CLKSTATUS, 0xffffffff);
    g_assert_cmphex(cpg_read(qts, CLKSTATUS), ==, 0);

    qtest_quit(qts);
}

/* A system reset brings back the reset values */
static void test_system_reset(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    cpg_write(qts, CLKON_GTM, WEN(0x7) | 0x6);
    cpg_write(qts, RST_SCIF, WEN(0x1f) | 0x1f);
    cpg_write(qts, PL1_DDIV, WEN(0x1) | 0x2);
    qtest_system_reset(qts);
    check_values(qts, reset_values, ARRAY_SIZE(reset_values));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rzv2l-smarc/cpg/reset-values", test_reset_values);
    qtest_add_func("/rzv2l-smarc/cpg/direct-boot", test_direct_boot);
    qtest_add_func("/rzv2l-smarc/cpg/write-enable", test_write_enable);
    qtest_add_func("/rzv2l-smarc/cpg/monitors", test_monitors);
    qtest_add_func("/rzv2l-smarc/cpg/system-reset", test_system_reset);

    return g_test_run();
}
