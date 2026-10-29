/*
 * QTest testcase for the Renesas RZ/V2L SMARC EVK machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Expected values come from the hardware documentation, never from the
 * model: the RZ/V2L Group User's Manual: Hardware (R01UH0936EJ0150) and
 * the Arm CoreLink GIC-600 TRM r1p6 (100336_0106_00). Where the manual
 * leaves a value to the part or its pins (initial value "-"), the value
 * read on the EVK in its BL2 boot prints is used and marked as such.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* System Controller (manual section 6) */
#define SYSC_BASE           0x11020000
#define SYS_LSI_MODE        0xa00
#define SYS_LSI_DEVID       0xa04
#define SYS_LSI_PRR         0xa08

/* GIC-600 (manual section 8, memory map Table 5.1) */
#define GICD_BASE           0x11900000
#define GICR_BASE           0x11940000
#define GICR_FRAME_SIZE     0x20000     /* RD_base + SGI_base, 64 KiB each */

#define GICD_TYPER          0x0004
#define GICD_IIDR           0x0008
#define GICR_IIDR           0x0004
#define GICR_TYPER          0x0008
#define GIC_PIDR4           0xffd0      /* PIDR4..7, PIDR0..3, CIDR0..3 */

/*
 * SYS_LSI_MODE (6.3.42): STAT_SEC_EN (bit 16) is part-dependent and reads
 * 1 on the EVK; MD_OSCDRV (15:14) and MD_CLKS (12) are pins, 0 on the EVK.
 */
#define LSI_MODE_SEC_EN     (1u << 16)
#define LSI_MODE_MD_OSCDRV(x) ((x) << 14)
#define LSI_MODE_MD_CLKS    (1u << 12)
#define LSI_MODE_DEBUGEN    (1u << 9)

/*
 * SYS_LSI_PRR (6.3.44): bit 0 CA55_1CPU = 0 for the dual-core part; bits 8
 * and 4 are part-dependent. The EVK reads 0 for the whole register.
 */
#define LSI_PRR_RZV2L       0x0

/* SYS_LSI_DEVID: revision 1, device ID 0x8447447 (6.3.43, EVK BL2 log) */
#define LSI_DEVID_RZV2L     0x18447447

/* GIC-600 r1p6: ProductID 0x02, r1, p6, Arm (TRM Tables 4-5, 4-22) */
#define GIC600_IIDR         0x0201743b

/* PIDR4..7, PIDR0..3, CIDR0..3 (TRM Tables 4-2, 4-21) */
static const uint8_t gicd_id[] = {
    0x44, 0x00, 0x00, 0x00, 0x92, 0xb4, 0x3b, 0x00, 0x0d, 0xf0, 0x05, 0xb1,
};
static const uint8_t gicr_id[] = {
    0x44, 0x00, 0x00, 0x00, 0x93, 0xb4, 0x3b, 0x00, 0x0d, 0xf0, 0x05, 0xb1,
};

/* MPIDR affinity of the two cores: DT cpu@0 and cpu@100 */
static const uint64_t cpu_affinity[] = { 0x0, 0x100 };

static void test_sysc_id(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    g_assert_cmphex(qtest_readl(qts, SYSC_BASE + SYS_LSI_DEVID), ==,
                    LSI_DEVID_RZV2L);
    g_assert_cmphex(qtest_readl(qts, SYSC_BASE + SYS_LSI_PRR), ==,
                    LSI_PRR_RZV2L);
    /* Default boot mode 0 (eSD), DEBUGEN low */
    g_assert_cmphex(qtest_readl(qts, SYSC_BASE + SYS_LSI_MODE), ==,
                    LSI_MODE_SEC_EN);

    /* The ID registers are read-only */
    qtest_writel(qts, SYSC_BASE + SYS_LSI_DEVID, 0);
    g_assert_cmphex(qtest_readl(qts, SYSC_BASE + SYS_LSI_DEVID), ==,
                    LSI_DEVID_RZV2L);
    qtest_quit(qts);
}

static void test_sysc_boot_pins(void)
{
    QTestState *qts;

    /* QSPI 1.8 V (Table 4.1) */
    qts = qtest_init("-machine rzv2l-smarc,boot-mode=3");
    g_assert_cmphex(qtest_readl(qts, SYSC_BASE + SYS_LSI_MODE), ==,
                    LSI_MODE_SEC_EN | 3);
    qtest_quit(qts);

    qts = qtest_init("-machine rzv2l-smarc,debugen=on");
    g_assert_cmphex(qtest_readl(qts, SYSC_BASE + SYS_LSI_MODE), ==,
                    LSI_MODE_SEC_EN | LSI_MODE_DEBUGEN);
    qtest_quit(qts);

    /* The clock straps: MD_OSCDRV[1:0] at 15:14, MD_CLKS at 12 (6.3.42) */
    qts = qtest_init("-machine rzv2l-smarc "
                     "-global renesas-rzg2l-sysc.md-oscdrv=2 "
                     "-global renesas-rzg2l-sysc.md-clks=on");
    g_assert_cmphex(qtest_readl(qts, SYSC_BASE + SYS_LSI_MODE), ==,
                    LSI_MODE_SEC_EN | LSI_MODE_MD_OSCDRV(2) |
                    LSI_MODE_MD_CLKS);
    qtest_quit(qts);
}

static void test_sysc_boot_mode_reserved(void)
{
    if (g_test_subprocess()) {
        /* Boot modes 6 and 7 are reserved (Table 4.1): QEMU must not start */
        QTestState *qts = qtest_init("-machine rzv2l-smarc,boot-mode=6");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*boot-mode 6 is reserved*");
}

static void test_gic_layout(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    uint32_t typer = qtest_readl(qts, GICD_BASE + GICD_TYPER);
    int i;

    /* 512 interrupt IDs: ITLinesNumber = 512 / 32 - 1 */
    g_assert_cmpuint(typer & 0x1f, ==, 15);
    /* No LPIs (LPIS, bit 17) */
    g_assert_cmpuint((typer >> 17) & 1, ==, 0);

    for (i = 0; i < ARRAY_SIZE(cpu_affinity); i++) {
        uint64_t rtyper = qtest_readq(qts, GICR_BASE + i * GICR_FRAME_SIZE +
                                      GICR_TYPER);

        /* Affinity_Value, bits [63:32] */
        g_assert_cmphex(rtyper >> 32, ==, cpu_affinity[i]);
        /* Last (bit 4): set only on the last redistributor */
        g_assert_cmpuint((rtyper >> 4) & 1, ==,
                         i == ARRAY_SIZE(cpu_affinity) - 1);
    }
    qtest_quit(qts);
}

static void check_id_block(QTestState *qts, uint64_t base,
                           const uint8_t *id)
{
    int i;

    for (i = 0; i < 12; i++) {
        g_assert_cmphex(qtest_readl(qts, base + GIC_PIDR4 + i * 4), ==, id[i]);
    }
}

static void test_gic_identity(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    int i;

    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_IIDR), ==,
                    GIC600_IIDR);
    check_id_block(qts, GICD_BASE, gicd_id);

    for (i = 0; i < ARRAY_SIZE(cpu_affinity); i++) {
        uint64_t rd_base = GICR_BASE + i * GICR_FRAME_SIZE;

        g_assert_cmphex(qtest_readl(qts, rd_base + GICR_IIDR), ==,
                        GIC600_IIDR);
        check_id_block(qts, rd_base, gicr_id);
    }
    qtest_quit(qts);
}

/*
 * Stubbed blocks: the DMA controller keeps what the driver writes
 * (DMA channel 0 CHCTRL at +0x28, DMARS at +0x0 of the second window);
 * the USB PHY control and thermal sensor windows read as zero.
 */
static void test_stubs(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    qtest_writel(qts, 0x11820028, 0x0002027a);
    g_assert_cmphex(qtest_readl(qts, 0x11820028), ==, 0x0002027a);
    qtest_writel(qts, 0x11830000, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, 0x11830000), ==, 0x12345678);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, 0x11820028), ==, 0);

    qtest_writel(qts, 0x11c40000, 0x303);
    g_assert_cmphex(qtest_readl(qts, 0x11c40000), ==, 0);
    qtest_writel(qts, 0x10059404, 0x1);
    g_assert_cmphex(qtest_readl(qts, 0x10059404), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rzv2l-smarc/sysc/id", test_sysc_id);
    qtest_add_func("/rzv2l-smarc/sysc/boot-pins", test_sysc_boot_pins);
    qtest_add_func("/rzv2l-smarc/sysc/boot-mode-reserved",
                   test_sysc_boot_mode_reserved);
    qtest_add_func("/rzv2l-smarc/gic/layout", test_gic_layout);
    qtest_add_func("/rzv2l-smarc/gic/identity", test_gic_identity);
    qtest_add_func("/rzv2l-smarc/stubs", test_stubs);

    return g_test_run();
}
