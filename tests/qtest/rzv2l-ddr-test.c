/*
 * QTest testcase for the DDR memory controller and PHY stub of the
 * Renesas RZ/V2L
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The RZ/V2L Group User's Manual: Hardware (R01UH0936EJ0150) documents
 * neither block's registers, so these are stub-contract tests: each
 * expected value is what TF-A's BL2 (plat/renesas/rz/common/drivers/ddr/
 * ddr.c, soc/v2l/drivers/ddr/ddr_v2l.c) polls for or reads back, not a
 * claim about the silicon. ECC_ENABLE's low bits are the value read on
 * the RZ/V2L SMARC EVK at the U-Boot prompt (0x00000fff). Refused
 * accesses (bus errors) need a CPU to observe and are not tested here;
 * the tests check only that a refused command changes nothing.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MC_BASE             0x11410000
#define PHY_BASE            0x11400000

#define MC_CTL00            0x000
#define MC_LP_CMD           0x0e8
#define MC_LP_STATE         0x0f4
#define MC_MRW              0x114
#define MC_ECC_ENABLE       0x18c
#define MC_ST_LOWPOWER      0x24c
#define MC_ST_INIT          0x260
#define MC_ST_MODE          0x264
#define MC_ACK_LOWPOWER     0x26c
#define MC_ACK_BIST         0x27c
#define MC_ACK_INIT         0x280
#define MC_ACK_MODE         0x284
#define MC_INIT_DONE        (1u << 25)
#define MC_MRW_DONE         (1u << 3)

#define PHY_SCL_START       0x100
#define PHY_WRLVL_CTRL      0x148
#define PHY_WRLVL_TRIM      0x14c
#define PHY_DLL_2           0x160
#define PHY_IO_1            0x170
#define PHY_VREF_WINDOWS    0x1d4

static uint32_t mc_read(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, MC_BASE + offset);
}

static void mc_write(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, MC_BASE + offset, value);
}

static uint32_t phy_read(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, PHY_BASE + offset);
}

static void phy_write(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, PHY_BASE + offset, value);
}

/* Start values: stored registers 0, the computed ones BL2's answers */
static void test_reset(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    g_assert_cmphex(mc_read(qts, MC_CTL00), ==, 0);
    g_assert_cmphex(mc_read(qts, 0x004), ==, 0);
    g_assert_cmphex(mc_read(qts, 0x7f8), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_ST_MODE), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_LP_STATE), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_ECC_ENABLE), ==, 0x00000fff);
    g_assert_cmphex(phy_read(qts, 0x058), ==, 0);
    g_assert_cmphex(phy_read(qts, 0x240), ==, 0);
    g_assert_cmphex(phy_read(qts, PHY_SCL_START), ==, 0x00000003);
    g_assert_cmphex(phy_read(qts, PHY_WRLVL_CTRL), ==, 0x00000003);
    g_assert_cmphex(phy_read(qts, PHY_DLL_2), ==, 0x00000003);
    g_assert_cmphex(phy_read(qts, PHY_VREF_WINDOWS), ==, 0);
    g_assert_cmphex(phy_read(qts, PHY_IO_1), ==, 0);
    qtest_quit(qts);
}

/* Stored registers read back what was written; a reset clears them */
static void test_storage(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    mc_write(qts, 0x004, 0xa5a5f00f);
    mc_write(qts, 0x22c, 0x12345678);
    mc_write(qts, 0x7f8, 0xfedcba98);
    mc_write(qts, MC_MRW, 0x00010203);
    mc_write(qts, MC_ECC_ENABLE, 0x03000000);
    phy_write(qts, 0x058, 0x0badcafe);
    phy_write(qts, PHY_WRLVL_TRIM, 0x76543210);
    phy_write(qts, 0x240, 0x80000001);
    g_assert_cmphex(mc_read(qts, 0x004), ==, 0xa5a5f00f);
    g_assert_cmphex(mc_read(qts, 0x22c), ==, 0x12345678);
    g_assert_cmphex(mc_read(qts, 0x7f8), ==, 0xfedcba98);
    g_assert_cmphex(mc_read(qts, MC_MRW), ==, 0x00010203);
    g_assert_cmphex(mc_read(qts, MC_ECC_ENABLE), ==, 0x03000fff);
    g_assert_cmphex(phy_read(qts, 0x058), ==, 0x0badcafe);
    g_assert_cmphex(phy_read(qts, PHY_WRLVL_TRIM), ==, 0x76543210);
    g_assert_cmphex(phy_read(qts, 0x240), ==, 0x80000001);

    qtest_system_reset(qts);
    g_assert_cmphex(mc_read(qts, 0x004), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_ECC_ENABLE), ==, 0x00000fff);
    g_assert_cmphex(phy_read(qts, 0x058), ==, 0);
    qtest_quit(qts);
}

/* A system reset clears the computed state too */
static void test_reset_state(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    mc_write(qts, MC_CTL00, 1);
    mc_write(qts, MC_MRW, 1u << 25);
    mc_write(qts, MC_LP_CMD, 0x00001100);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, MC_INIT_DONE);
    g_assert_cmphex(mc_read(qts, MC_ST_MODE), ==, MC_MRW_DONE);
    g_assert_cmphex(mc_read(qts, MC_LP_STATE), ==, 0x48000000);

    qtest_system_reset(qts);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_ST_MODE), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_LP_STATE), ==, 0);
    qtest_quit(qts);
}

/* START sets init-done; its acknowledgement or START = 0 clears it */
static void test_init_done(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    mc_write(qts, MC_CTL00, 0x00000b00);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, 0);
    mc_write(qts, MC_CTL00, 0x00000b01);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, MC_INIT_DONE);
    g_assert_cmphex(mc_read(qts, MC_CTL00), ==, 0x00000b01);

    /* The acknowledgement reads 0 and clears the status */
    mc_write(qts, MC_ACK_INIT, MC_INIT_DONE);
    g_assert_cmphex(mc_read(qts, MC_ACK_INIT), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, 0);

    /* START already set: rewriting it is not a new start */
    mc_write(qts, MC_CTL00, 0x00000b01);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, 0);

    /* START cleared clears the status */
    mc_write(qts, MC_CTL00, 0);
    mc_write(qts, MC_CTL00, 1);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, MC_INIT_DONE);
    mc_write(qts, MC_CTL00, 0);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, 0);
    qtest_quit(qts);
}

/* A mode-register request sets its done bit; the acknowledgement clears */
static void test_mode_register(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    mc_write(qts, MC_MRW, 0x00800000);
    g_assert_cmphex(mc_read(qts, MC_ST_MODE), ==, 0);
    mc_write(qts, MC_MRW, 0x02810000);
    g_assert_cmphex(mc_read(qts, MC_ST_MODE), ==, MC_MRW_DONE);
    g_assert_cmphex(mc_read(qts, MC_MRW), ==, 0x02810000);
    mc_write(qts, MC_ACK_MODE, MC_MRW_DONE);
    g_assert_cmphex(mc_read(qts, MC_ACK_MODE), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_ST_MODE), ==, 0);
    qtest_quit(qts);
}

/* LP_CMD 0x11 and 0x02 set LP_STATE; 0 and a refused command do not */
static void test_low_power(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    mc_write(qts, MC_LP_STATE, 0x80000005);
    g_assert_cmphex(mc_read(qts, MC_LP_STATE), ==, 0x80000005);

    mc_write(qts, MC_LP_CMD, 0x00001100);
    g_assert_cmphex(mc_read(qts, MC_LP_STATE), ==, 0xc8000005);
    g_assert_cmphex(mc_read(qts, MC_LP_CMD), ==, 0x00001100);
    mc_write(qts, MC_LP_CMD, 0);
    g_assert_cmphex(mc_read(qts, MC_LP_STATE), ==, 0xc8000005);
    mc_write(qts, MC_LP_CMD, 0x00000200);
    g_assert_cmphex(mc_read(qts, MC_LP_STATE), ==, 0xc0000005);

    /* LP_STATE's state bits ignore writes; the others are stored */
    mc_write(qts, MC_LP_STATE, 0x7f000000);
    g_assert_cmphex(mc_read(qts, MC_LP_STATE), ==, 0x40000000);

    /* A refused command is not stored and leaves the state */
    mc_write(qts, MC_LP_CMD, 0x00000500);
    g_assert_cmphex(mc_read(qts, MC_LP_CMD), ==, 0x00000200);
    g_assert_cmphex(mc_read(qts, MC_LP_STATE), ==, 0x40000000);

    /* The low-power status's upper half reads 0 */
    mc_write(qts, MC_ST_LOWPOWER, 0xffffffff);
    g_assert_cmphex(mc_read(qts, MC_ST_LOWPOWER), ==, 0x0000ffff);
    qtest_quit(qts);
}

/* Status and acknowledgement registers ignore writes (no bus error) */
static void test_read_only(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    mc_write(qts, MC_ST_INIT, MC_INIT_DONE);
    mc_write(qts, MC_ST_MODE, MC_MRW_DONE);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_ST_MODE), ==, 0);
    mc_write(qts, MC_CTL00, 1);
    mc_write(qts, MC_MRW, 1u << 25);
    mc_write(qts, MC_ST_INIT, 0);
    mc_write(qts, MC_ST_MODE, 0);
    g_assert_cmphex(mc_read(qts, MC_ST_INIT), ==, MC_INIT_DONE);
    g_assert_cmphex(mc_read(qts, MC_ST_MODE), ==, MC_MRW_DONE);
    mc_write(qts, MC_ACK_LOWPOWER, 0xffffffff);
    mc_write(qts, MC_ACK_BIST, 0xffffffff);
    g_assert_cmphex(mc_read(qts, MC_ACK_LOWPOWER), ==, 0);
    g_assert_cmphex(mc_read(qts, MC_ACK_BIST), ==, 0);

    phy_write(qts, PHY_WRLVL_CTRL, 0);
    phy_write(qts, PHY_DLL_2, 0);
    phy_write(qts, PHY_VREF_WINDOWS, 0xffffffff);
    g_assert_cmphex(phy_read(qts, PHY_WRLVL_CTRL), ==, 0x00000003);
    g_assert_cmphex(phy_read(qts, PHY_DLL_2), ==, 0x00000003);
    g_assert_cmphex(phy_read(qts, PHY_VREF_WINDOWS), ==, 0);
    qtest_quit(qts);
}

/* BL2's training commands complete at once; write leveling clears 0x14c */
static void test_training(void)
{
    /* TF-A ddr.c: write leveling, the VREF, bit and SL training passes */
    static const uint32_t commands[] = {
        0x50200000, 0x30800000, 0x30500000, 0x30a00000, 0x30700000,
        0x11200000, 0x34200000,
    };
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    int i;

    phy_write(qts, PHY_WRLVL_TRIM, 0xffffffff);
    phy_write(qts, PHY_SCL_START, 0x30800000);
    g_assert_cmphex(phy_read(qts, PHY_SCL_START), ==, 0x20800003);
    g_assert_cmphex(phy_read(qts, PHY_WRLVL_TRIM), ==, 0xffffffff);
    phy_write(qts, PHY_SCL_START, 0x50200000);
    g_assert_cmphex(phy_read(qts, PHY_SCL_START), ==, 0x40200003);
    g_assert_cmphex(phy_read(qts, PHY_WRLVL_TRIM), ==, 0xfffffffc);

    /* Each of BL2's seven commands completes */
    for (i = 0; i < ARRAY_SIZE(commands); i++) {
        phy_write(qts, PHY_SCL_START, commands[i]);
        g_assert_cmphex(phy_read(qts, PHY_SCL_START), ==,
                        (commands[i] & ~(1u << 28)) | 3);
    }

    /* A value without the start bit is stored */
    phy_write(qts, PHY_SCL_START, 0x00000004);
    g_assert_cmphex(phy_read(qts, PHY_SCL_START), ==, 0x00000007);

    /* An unknown command is refused: not stored, 0x14c untouched */
    phy_write(qts, PHY_WRLVL_TRIM, 0xffffffff);
    phy_write(qts, PHY_SCL_START, 0x50200001);
    g_assert_cmphex(phy_read(qts, PHY_SCL_START), ==, 0x00000007);
    g_assert_cmphex(phy_read(qts, PHY_WRLVL_TRIM), ==, 0xffffffff);
    qtest_quit(qts);
}

/* 0x170 bits 3:2 mirror bits 1:0, and keep doing so after a RMW */
static void test_io_calibration(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    uint32_t v;

    phy_write(qts, PHY_IO_1, 0x00000001);
    g_assert_cmphex(phy_read(qts, PHY_IO_1), ==, 0x00000005);
    phy_write(qts, PHY_IO_1, 0x00000003);
    g_assert_cmphex(phy_read(qts, PHY_IO_1), ==, 0x0000000f);

    v = phy_read(qts, PHY_IO_1);
    phy_write(qts, PHY_IO_1, (v & ~0x3u) | 0x100);
    g_assert_cmphex(phy_read(qts, PHY_IO_1), ==, 0x00000100);
    v = phy_read(qts, PHY_IO_1);
    phy_write(qts, PHY_IO_1, v | 0x2);
    g_assert_cmphex(phy_read(qts, PHY_IO_1), ==, 0x0000010a);

    /* Bits 3:2 alone are not stored */
    phy_write(qts, PHY_IO_1, 0x0000000c);
    g_assert_cmphex(phy_read(qts, PHY_IO_1), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rzv2l-ddr/reset", test_reset);
    qtest_add_func("/rzv2l-ddr/storage", test_storage);
    qtest_add_func("/rzv2l-ddr/reset-state", test_reset_state);
    qtest_add_func("/rzv2l-ddr/init-done", test_init_done);
    qtest_add_func("/rzv2l-ddr/mode-register", test_mode_register);
    qtest_add_func("/rzv2l-ddr/low-power", test_low_power);
    qtest_add_func("/rzv2l-ddr/read-only", test_read_only);
    qtest_add_func("/rzv2l-ddr/training", test_training);
    qtest_add_func("/rzv2l-ddr/io-calibration", test_io_calibration);

    return g_test_run();
}
