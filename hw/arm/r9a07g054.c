/*
 * Renesas RZ/V2L (R9A07G054) SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Two Cortex-A55 cores, a GIC-600 (modelled by the GICv3), the Clock
 * Pulse Generator, the System Controller identification registers, the
 * five SCIFA serial ports, the three OS timers, the three watchdog
 * timers, the pin function controller and the interrupt control unit's
 * registers. A few blocks
 * that a kernel probes are stubbed (see r9a07g054_unimp). Everything
 * else in the address space is left unmapped, so guest accesses to
 * blocks that are not modelled yet fault instead of reading as zero.
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/arm/bsa.h"
#include "hw/arm/r9a07g054.h"
#include "hw/core/qdev-properties.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "qapi/error.h"
#include "qobject/qlist.h"
#include "system/system.h"
#include "hw/misc/unimp.h"

enum {
    R9A07G054_SCIF0,
    R9A07G054_CPG,
    R9A07G054_SYSC,
    R9A07G054_PFC,
    R9A07G054_IRQC,
    R9A07G054_OSTM0,
    R9A07G054_GIC_DIST,
    R9A07G054_GIC_REDIST,
};

static const hwaddr r9a07g054_memmap[] = {
    [R9A07G054_SCIF0]       = 0x1004b800,   /* SCIFn at + n * 0x400 */
    [R9A07G054_CPG]         = 0x11010000,
    [R9A07G054_SYSC]        = 0x11020000,
    [R9A07G054_PFC]         = 0x11030000,
    [R9A07G054_IRQC]        = 0x110a0000,
    [R9A07G054_OSTM0]       = 0x12801000,   /* OSTMn at + n * 0x400 */
    [R9A07G054_GIC_DIST]    = 0x11900000,
    [R9A07G054_GIC_REDIST]  = 0x11940000,
};

/* MPIDR affinity of each core: cpu@0 and cpu@100 in the device tree */
static const uint64_t r9a07g054_mp_affinity[R9A07G054_NUM_CPUS] = {
    0x0, 0x100,
};

/* Generic timer frequency, as TF-A programs it from SYC CNTFID0 */
#define R9A07G054_CNTFRQ_HZ     24000000

/*
 * SYSC identification values of the RZ/V2L, as read on the EVK (BL2
 * prints, every boot): SYS_LSI_DEVID 0x18447447 (revision 1, device ID
 * 0x8447447); SYS_LSI_PRR 0 (dual Cortex-A55; bits 8 and 4 are
 * part-dependent in the manual and read 0); SYS_LSI_MODE.STAT_SEC_EN 1.
 * The EVK straps MD_CLKS and MD_OSCDRV[1:0] low (they read 0), which is
 * the SYSC default, so they are not set here.
 */
#define R9A07G054_LSI_DEVID     0x18447447
#define R9A07G054_LSI_PRR       0x0
#define R9A07G054_LSI_SEC_EN    true

/* GICD_IIDR/GICR_IIDR of the Arm GIC-600 r1p6 */
#define R9A07G054_GIC_IIDR      0x0201743b

/* SCIF0 ERI, BRI, RXI, TXI, TEI/DRI are SPI 380-384; SCIFn follow */
#define R9A07G054_SCIF0_SPI     380

/* OSTM0-2 interrupts are SPI 46-48 */
#define R9A07G054_OSTM0_SPI     46

/* WDTn: WDTINT is SPI 49 + 2n, PERROUT SPI 50 + 2n (Table 8.2) */
static const hwaddr r9a07g054_wdt_base[R9A07G054_NUM_WDT] = {
    0x12800800, 0x12800c00, 0x12800400,
};
#define R9A07G054_WDT0_SPI      49

/* SDHIn: OXMNIRQ is SPI 104 + 2n, OXASIOIRQ SPI 105 + 2n (Table 8.2) */
static const hwaddr r9a07g054_sdhi_base[R9A07G054_NUM_SDHI] = {
    0x11c00000, 0x11c10000,
};
#define R9A07G054_SDHI0_SPI     104

/* ETHn: pif_int_n, int_fil_n, int_arp_ns_n are SPI 84-86 + 3n (Table 8.2) */
static const hwaddr r9a07g054_eth_base[R9A07G054_NUM_ETH] = {
    0x11c20000, 0x11c30000,
};
#define R9A07G054_ETH0_SPI      84

/* IRQ0-7 are SPI 1-8 (Table 8.2) */
#define R9A07G054_IRQ0_SPI      1

/* WDT counter clock WDTn_CLK (manual section 21.1.1) */
#define R9A07G054_WDT_CLK_HZ    24000000

/*
 * OSTM count clock P0 (manual section 20.3.2): PLL2 1600 MHz / 2 / 8 with
 * CPG_PL2_DDIV.DIVPL2A_SET = 0, its reset value and the value the EVK
 * firmware leaves.
 */
#define R9A07G054_P0_HZ         100000000

/*
 * Blocks that are not modelled but that a kernel probes. Each one logs
 * every access. Reads return 0, or with retain_writes the last value
 * written: the DMA controller driver writes its channel registers at
 * probe and only reads them back to run a transfer; the USB PHY control
 * driver sets and clears reset bits with read-modify-writes.
 */
static const struct {
    const char *name;
    hwaddr base;
    hwaddr size;
    bool retain_writes;
} r9a07g054_unimp[] = {
    { "rz-dmac",        0x11820000, 0x10000, true },
    { "rz-dmac-ext",    0x11830000, 0x10000, true },
    { "usbphy-ctrl",    0x11c40000, 0x10000, true },
    { "tsu",            0x10059400, 0x400,   false },
    { "riic0",          0x10058000, 0x400,   false },
    { "riic1",          0x10058400, 0x400,   false },
    { "riic3",          0x10058c00, 0x400,   false },
    { "usb0-host",      0x11c50000, 0x10000, false },
    { "usb1-host",      0x11c70000, 0x10000, false },
    { "mipi-dsi",       0x10850000, 0x20000, false },
    { "vspd",           0x10870000, 0x10000, false },
    { "fcpvd",          0x10880000, 0x10000, false },
    { "du",             0x10890000, 0x10000, false },
    { "ssi0",           0x10049c00, 0x400,   false },
    { "ssi1",           0x1004a000, 0x400,   false },
    { "rspi1",          0x1004b000, 0x400,   false },
    { "canfd",          0x10050000, 0x8000,  false },
    { "adc",            0x10059000, 0x400,   false },
    { "usb0-func",      0x11c60000, 0x10000, false },
    { "rpc-if",         0x10060000, 0x10000, false },
    { "rpc-if-wbuf",    0x10070000, 0x10000, false },
    { "rpc-if-dirmap",  0x20000000, 0x10000000, false },
};

static void r9a07g054_init(Object *obj)
{
    R9A07G054State *s = R9A07G054(obj);
    int i;

    for (i = 0; i < R9A07G054_NUM_CPUS; i++) {
        g_autofree char *name = g_strdup_printf("cpu%d", i);
        object_initialize_child(obj, name, &s->cpu[i],
                                ARM_CPU_TYPE_NAME("cortex-a55"));
    }

    object_initialize_child(obj, "gic", &s->gic, gicv3_class_name());

    object_initialize_child(obj, "cpg", &s->cpg, TYPE_RENESAS_RZV2L_CPG);
    object_property_add_alias(obj, "cpg-direct-boot", OBJECT(&s->cpg),
                              "direct-boot");

    object_initialize_child(obj, "sysc", &s->sysc, TYPE_RENESAS_RZG2L_SYSC);
    object_property_add_alias(obj, "boot-mode", OBJECT(&s->sysc),
                              "boot-mode");
    object_property_add_alias(obj, "debugen", OBJECT(&s->sysc), "debugen");

    for (i = 0; i < R9A07G054_NUM_SCIF; i++) {
        g_autofree char *name = g_strdup_printf("scif%d", i);

        object_initialize_child(obj, name, &s->scif[i], TYPE_RENESAS_RZ_SCIFA);
    }
    object_property_add_alias(obj, "scif0-direct-boot", OBJECT(&s->scif[0]),
                              "direct-boot");

    object_initialize_child(obj, "irqc", &s->irqc, TYPE_RENESAS_RZG2L_IRQC);

    object_initialize_child(obj, "pfc", &s->pfc, TYPE_RENESAS_RZG2L_PFC);
    object_property_add_alias(obj, "pfc-direct-boot", OBJECT(&s->pfc),
                              "direct-boot");

    for (i = 0; i < R9A07G054_NUM_OSTM; i++) {
        g_autofree char *name = g_strdup_printf("ostm%d", i);

        object_initialize_child(obj, name, &s->ostm[i], TYPE_RENESAS_RZ_OSTM);
    }

    for (i = 0; i < R9A07G054_NUM_WDT; i++) {
        g_autofree char *name = g_strdup_printf("wdt%d", i);

        object_initialize_child(obj, name, &s->wdt[i], TYPE_RENESAS_RZG2L_WDT);
    }

    for (i = 0; i < R9A07G054_NUM_SDHI; i++) {
        g_autofree char *name = g_strdup_printf("sdhi%d", i);
        g_autofree char *bus = g_strdup_printf("sd-bus%d", i);

        object_initialize_child(obj, name, &s->sdhi[i], TYPE_RENESAS_SDHI);
        object_property_add_alias(obj, bus, OBJECT(&s->sdhi[i]), "sd-bus");
    }

    for (i = 0; i < R9A07G054_NUM_ETH; i++) {
        g_autofree char *name = g_strdup_printf("eth%d", i);

        object_initialize_child(obj, name, &s->eth[i], TYPE_RENESAS_GBETH);
    }
}

static void r9a07g054_realize(DeviceState *dev, Error **errp)
{
    R9A07G054State *s = R9A07G054(dev);
    DeviceState *gicdev = DEVICE(&s->gic);
    SysBusDevice *gicsbd = SYS_BUS_DEVICE(&s->gic);
    QList *redist_region_count;
    int i;

    /*
     * CPUs. They must be realized before the GIC, which copies each
     * CPU's affinity into GICR_TYPER when it is realized.
     */
    for (i = 0; i < R9A07G054_NUM_CPUS; i++) {
        Object *cpuobj = OBJECT(&s->cpu[i]);

        object_property_set_int(cpuobj, "mp-affinity",
                                r9a07g054_mp_affinity[i], &error_abort);
        object_property_set_int(cpuobj, "cntfrq", R9A07G054_CNTFRQ_HZ,
                                &error_abort);
        object_property_set_bool(cpuobj, "has_el3", true, &error_abort);
        object_property_set_bool(cpuobj, "has_el2", false, &error_abort);
        if (i) {
            /* Secondary cores stay off until powered on through PSCI */
            object_property_set_bool(cpuobj, "start-powered-off", true,
                                     &error_abort);
        }
        if (!qdev_realize(DEVICE(cpuobj), NULL, errp)) {
            return;
        }
    }

    /* GIC */
    qdev_prop_set_uint32(gicdev, "num-cpu", R9A07G054_NUM_CPUS);
    qdev_prop_set_uint32(gicdev, "num-irq",
                         R9A07G054_NUM_SPIS + GIC_INTERNAL);
    qdev_prop_set_uint32(gicdev, "revision", 3);
    qdev_prop_set_uint32(gicdev, "iidr", R9A07G054_GIC_IIDR);
    qdev_prop_set_bit(gicdev, "has-security-extensions", true);
    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, R9A07G054_NUM_CPUS);
    qdev_prop_set_array(gicdev, "redist-region-count", redist_region_count);
    if (!sysbus_realize(gicsbd, errp)) {
        return;
    }
    sysbus_mmio_map(gicsbd, 0, r9a07g054_memmap[R9A07G054_GIC_DIST]);
    sysbus_mmio_map(gicsbd, 1, r9a07g054_memmap[R9A07G054_GIC_REDIST]);

    /*
     * Wire each CPU's generic timers, GIC maintenance interrupt and PMU
     * interrupt to its PPIs, and the GIC's outputs to the CPU's inputs.
     */
    for (i = 0; i < R9A07G054_NUM_CPUS; i++) {
        DeviceState *cpudev = DEVICE(&s->cpu[i]);
        int intidbase = R9A07G054_NUM_SPIS + i * GIC_INTERNAL;
        static const int timer_irqs[] = {
            [GTIMER_PHYS]       = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT]       = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP]        = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC]        = ARCH_TIMER_S_EL1_IRQ,
            [GTIMER_HYPVIRT]    = ARCH_TIMER_NS_EL2_VIRT_IRQ,
        };
        qemu_irq irq;
        int j;

        for (j = 0; j < ARRAY_SIZE(timer_irqs); j++) {
            irq = qdev_get_gpio_in(gicdev, intidbase + timer_irqs[j]);
            qdev_connect_gpio_out(cpudev, j, irq);
        }

        irq = qdev_get_gpio_in(gicdev, intidbase + ARCH_GIC_MAINT_IRQ);
        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt",
                                    0, irq);

        irq = qdev_get_gpio_in(gicdev, intidbase + VIRTUAL_PMU_IRQ);
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0, irq);

        sysbus_connect_irq(gicsbd, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicsbd, i + R9A07G054_NUM_CPUS,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicsbd, i + 2 * R9A07G054_NUM_CPUS,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicsbd, i + 3 * R9A07G054_NUM_CPUS,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }

    /* CPG */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->cpg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->cpg), 0,
                    r9a07g054_memmap[R9A07G054_CPG]);

    /* SYSC */
    qdev_prop_set_uint32(DEVICE(&s->sysc), "lsi-devid", R9A07G054_LSI_DEVID);
    qdev_prop_set_uint32(DEVICE(&s->sysc), "lsi-prr", R9A07G054_LSI_PRR);
    qdev_prop_set_bit(DEVICE(&s->sysc), "sec-en", R9A07G054_LSI_SEC_EN);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sysc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysc), 0,
                    r9a07g054_memmap[R9A07G054_SYSC]);

    /* PFC */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pfc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pfc), 0,
                    r9a07g054_memmap[R9A07G054_PFC]);

    /* IRQC (IA55): IRQ0-7 to the GIC; NMI and TINT0-31 not routed */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->irqc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->irqc), 0,
                    r9a07g054_memmap[R9A07G054_IRQC]);
    for (i = 0; i < RENESAS_RZG2L_IRQC_NUM_IRQ; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->irqc), i,
                           qdev_get_gpio_in(gicdev, R9A07G054_IRQ0_SPI + i));
    }

    /* OSTM0-2 */
    for (i = 0; i < R9A07G054_NUM_OSTM; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->ostm[i]);

        qdev_prop_set_uint32(DEVICE(sbd), "clock-frequency", R9A07G054_P0_HZ);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, r9a07g054_memmap[R9A07G054_OSTM0] +
                        i * RENESAS_RZ_OSTM_MMIO_SIZE);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gicdev,
                           R9A07G054_OSTM0_SPI + i));
    }

    /* WDT0-2: reset lines to and from the CPG */
    for (i = 0; i < R9A07G054_NUM_WDT; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->wdt[i]);

        qdev_prop_set_uint32(DEVICE(sbd), "clock-frequency",
                             R9A07G054_WDT_CLK_HZ);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, r9a07g054_wdt_base[i]);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gicdev,
                           R9A07G054_WDT0_SPI + 2 * i));
        sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(gicdev,
                           R9A07G054_WDT0_SPI + 2 * i + 1));
        qdev_connect_gpio_out_named(DEVICE(sbd), "wdtrstb", 0,
                                    qdev_get_gpio_in_named(DEVICE(&s->cpg),
                                                           "wdtrstb", i));
        qdev_connect_gpio_out_named(DEVICE(&s->cpg), "wdt-presetn", i,
                                    qdev_get_gpio_in_named(DEVICE(sbd),
                                                           "presetn", 0));
    }

    /* SDHI0-1: the built-in DMAC sees the system address space */
    for (i = 0; i < R9A07G054_NUM_SDHI; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->sdhi[i]);

        /* SD_STATUS (the SD0_RST# pin) exists on channel 0 only */
        qdev_prop_set_bit(DEVICE(sbd), "has-sd-status", i == 0);
        /*
         * The SMARC module routes neither SD0_WP (its pin carries RIIC3)
         * nor SD1_WP (unused), and SD0_CD only in the SD pin mux, which
         * the board's device trees do not select (module UM Table 4.1,
         * p.20, p.25). SD1_CD comes from the carrier board's slot.
         */
        qdev_prop_set_bit(DEVICE(sbd), "cd-connected", i == 1);
        qdev_prop_set_bit(DEVICE(sbd), "wp-connected", false);
        object_property_set_link(OBJECT(sbd), "dma-memory",
                                 OBJECT(get_system_memory()), &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, r9a07g054_sdhi_base[i]);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gicdev,
                           R9A07G054_SDHI0_SPI + 2 * i));
        sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(gicdev,
                           R9A07G054_SDHI0_SPI + 2 * i + 1));
        qdev_connect_gpio_out_named(DEVICE(&s->cpg), "sdhi-rst", i,
                                    qdev_get_gpio_in_named(DEVICE(sbd),
                                                           "ixrst", 0));
    }

    /* ETH0-1: the DMAC sees the system address space */
    for (i = 0; i < R9A07G054_NUM_ETH; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->eth[i]);
        int j;

        object_property_set_link(OBJECT(sbd), "dma-memory",
                                 OBJECT(get_system_memory()), &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, r9a07g054_eth_base[i]);
        for (j = 0; j < RENESAS_GBETH_NUM_IRQ; j++) {
            sysbus_connect_irq(sbd, j, qdev_get_gpio_in(gicdev,
                               R9A07G054_ETH0_SPI + 3 * i + j));
        }
        qdev_connect_gpio_out_named(DEVICE(&s->cpg), "eth-rst", i,
                                    qdev_get_gpio_in_named(DEVICE(sbd),
                                                           "reset", 0));
    }

    /* Unimplemented blocks */
    for (i = 0; i < ARRAY_SIZE(r9a07g054_unimp); i++) {
        DeviceState *unimp = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);

        qdev_prop_set_string(unimp, "name", r9a07g054_unimp[i].name);
        qdev_prop_set_uint64(unimp, "size", r9a07g054_unimp[i].size);
        qdev_prop_set_bit(unimp, "retain-writes",
                          r9a07g054_unimp[i].retain_writes);
        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(unimp), errp)) {
            return;
        }
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(unimp), 0,
                                r9a07g054_unimp[i].base, -1000);
    }

    /* SCIF0-4 */
    for (i = 0; i < R9A07G054_NUM_SCIF; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->scif[i]);
        int j;

        qdev_prop_set_chr(DEVICE(sbd), "chardev", serial_hd(i));
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, r9a07g054_memmap[R9A07G054_SCIF0] +
                        i * RENESAS_RZ_SCIFA_MMIO_SIZE);
        for (j = 0; j < RENESAS_RZ_SCIFA_NUM_IRQS; j++) {
            sysbus_connect_irq(sbd, j, qdev_get_gpio_in(gicdev,
                               R9A07G054_SCIF0_SPI +
                               i * RENESAS_RZ_SCIFA_NUM_IRQS + j));
        }
    }
}

static void r9a07g054_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = r9a07g054_realize;
    dc->desc = "Renesas RZ/V2L (R9A07G054) SoC";
}

static const TypeInfo r9a07g054_types[] = {
    {
        .name = TYPE_R9A07G054,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(R9A07G054State),
        .instance_init = r9a07g054_init,
        .class_init = r9a07g054_class_init,
    },
};

DEFINE_TYPES(r9a07g054_types)
