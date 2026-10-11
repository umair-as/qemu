/*
 * QTest testcase for the GICv3 "iidr" property
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* virt machine GICv3 layout (hw/arm/virt.c) */
#define VIRT_GICD_BASE  0x08000000
#define VIRT_GICR_BASE  0x080a0000

#define GICD_IIDR       0x0008
#define GICR_IIDR       0x0004

/* QEMU's default: ARM r0p0 with a zero ProductID */
#define IIDR_DEFAULT    0x0000043b
/* Arm GIC-600 r1p6: ProductID 0x02, r1, p6, ARM (TRM 100336_0106_00) */
#define IIDR_GIC600_R1P6 0x0201743b

static void check_iidr(const char *extra_args, uint32_t expected)
{
    QTestState *qts;

    qts = qtest_initf("-machine virt,gic-version=3 -smp 2 %s", extra_args);
    g_assert_cmphex(qtest_readl(qts, VIRT_GICD_BASE + GICD_IIDR), ==,
                    expected);
    /* Both redistributors: 128 KiB frames for GICv3 */
    g_assert_cmphex(qtest_readl(qts, VIRT_GICR_BASE + GICR_IIDR), ==,
                    expected);
    g_assert_cmphex(qtest_readl(qts, VIRT_GICR_BASE + 0x20000 + GICR_IIDR),
                    ==, expected);
    qtest_quit(qts);
}

static void test_iidr_default(void)
{
    check_iidr("", IIDR_DEFAULT);
}

static void test_iidr_property(void)
{
    char *args = g_strdup_printf("-global arm-gicv3.iidr=0x%x",
                                 IIDR_GIC600_R1P6);

    check_iidr(args, IIDR_GIC600_R1P6);
    g_free(args);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/arm-gicv3/iidr/default", test_iidr_default);
    qtest_add_func("/arm-gicv3/iidr/property", test_iidr_property);

    return g_test_run();
}
