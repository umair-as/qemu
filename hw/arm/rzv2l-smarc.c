/*
 * Renesas RZ/V2L SMARC Evaluation Kit
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The SMARC module with 2 GiB of DDR4. A kernel loaded with -kernel is
 * entered at non-secure EL1, with QEMU's PSCI over SMC standing in for
 * TF-A, and is given the memory window that TF-A and U-Boot give Linux
 * on the board: the first 128 MiB of DDR belong to the secure firmware.
 *
 * Each Gigabit Ethernet channel has a KSZ9131 PHY on the module, at
 * MDIO address 7 (its strap pins pull PHYAD[2:0] high); its INT_N pin
 * drives IRQ2 (ETH0) or IRQ3 (ETH1), and its reset follows the board
 * reset (RZ/G2L, RZ/V2L SMARC module user's manual, section 4.3). The
 * first -nic is ETH0, the second ETH1; a channel without one has no
 * cable.
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/arm/boot.h"
#include "hw/arm/r9a07g054.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/sd.h"
#include "hw/net/ksz9131_phy.h"
#include "net/net.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "system/qtest.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include <libfdt.h>

#define TYPE_RZV2L_SMARC_MACHINE MACHINE_TYPE_NAME("rzv2l-smarc")
OBJECT_DECLARE_SIMPLE_TYPE(RZV2LSmarcState, RZV2L_SMARC_MACHINE)

#define RZV2L_SMARC_RAM_SIZE        (2 * GiB)

/* The DDR window TF-A and U-Boot hand to Linux */
#define RZV2L_SMARC_LINUX_MEM_BASE  0x48000000
#define RZV2L_SMARC_LINUX_MEM_SIZE  0x78000000

/* The PHY address straps, and the IRQn pin INT_N of each PHY drives */
#define RZV2L_SMARC_PHY_ADDR        7
static const int rzv2l_smarc_phy_irq[R9A07G054_NUM_ETH] = { 2, 3 };

/* MD_BOOT[2:0] = 0: boot from the SD card on the module (eSD) */
#define RZV2L_SMARC_BOOT_MODE_ESD   0

struct RZV2LSmarcState {
    MachineState parent_obj;

    R9A07G054State soc;
    KSZ9131PhyState phy[R9A07G054_NUM_ETH];
    struct arm_boot_info boot_info;

    /* Board switches, latched by the SoC at power-on */
    uint8_t boot_mode;
    bool debugen;
};

/*
 * Point a supply property of an existing node at a regulator node, as
 * the vendor U-Boot's update_fdt() does: the regulator gets a phandle
 * if it has none, and a missing regulator leaves the property as it was.
 */
static void rzv2l_smarc_set_supply(void *fdt, const char *node,
                                   const char *prop, const char *regulator)
{
    int node_off, reg_off;
    uint32_t phandle;

    node_off = fdt_path_offset(fdt, node);
    reg_off = fdt_path_offset(fdt, regulator);
    if (node_off < 0 || reg_off < 0) {
        return;
    }
    phandle = fdt_get_phandle(fdt, reg_off);
    if (!phandle) {
        if (fdt_generate_phandle(fdt, &phandle) < 0 ||
            fdt_setprop_u32(fdt, reg_off, "phandle", phandle) < 0) {
            error_report("couldn't add a phandle to %s", regulator);
            exit(1);
        }
        /* Adding a property may move the node */
        node_off = fdt_path_offset(fdt, node);
    }
    if (fdt_setprop_u32(fdt, node_off, prop, phandle) < 0) {
        error_report("couldn't set %s/%s", node, prop);
        exit(1);
    }
}

/*
 * Without "mmcN" aliases Linux numbers the SD hosts in probe order, and
 * the two channels probe in parallel, so the card's block device name
 * changes from boot to boot. Name SDHI0 mmc0 and SDHI1 mmc1 unless the
 * device tree already gives an alias of that name. This is QEMU's own
 * edit: the board's vendor U-Boot does not make it.
 */
static void rzv2l_smarc_add_mmc_aliases(void *fdt)
{
    static const char * const sdhi[] = {
        "/soc/mmc@11c00000", "/soc/mmc@11c10000",
    };
    int aliases, i;

    aliases = fdt_path_offset(fdt, "/aliases");
    if (aliases < 0) {
        aliases = fdt_add_subnode(fdt, 0, "aliases");
    }
    for (i = 0; i < ARRAY_SIZE(sdhi); i++) {
        g_autofree char *name = g_strdup_printf("mmc%d", i);

        if (fdt_path_offset(fdt, sdhi[i]) < 0 ||
            fdt_getprop(fdt, aliases, name, NULL)) {
            continue;
        }
        if (fdt_setprop_string(fdt, aliases, name, sdhi[i]) < 0) {
            error_report("couldn't add the /aliases/%s property", name);
            exit(1);
        }
        /* Adding a property may move the node */
        aliases = fdt_path_offset(fdt, "/aliases");
    }
}

/*
 * U-Boot's fdt_fixup_ethernet(): the node each "ethernetN" alias names
 * gets the MAC address of the channel in "local-mac-address", and in
 * "mac-address" if it has one. On the board the addresses come from
 * U-Boot's environment; here from the NICs.
 */
static void rzv2l_smarc_set_mac_addresses(RZV2LSmarcState *s, void *fdt)
{
    int i;

    for (i = 0; i < R9A07G054_NUM_ETH; i++) {
        g_autofree char *name = g_strdup_printf("ethernet%d", i);
        const uint8_t *mac = s->soc.eth[i].conf.macaddr.a;
        const char *path = fdt_get_alias(fdt, name);
        int node;

        node = path ? fdt_path_offset(fdt, path) : -1;
        if (node < 0) {
            continue;
        }
        if (fdt_getprop(fdt, node, "mac-address", NULL) &&
            fdt_setprop_inplace(fdt, node, "mac-address", mac, 6) < 0) {
            error_report("couldn't set %s/mac-address", path);
            exit(1);
        }
        if (fdt_setprop(fdt, node, "local-mac-address", mac, 6) < 0) {
            error_report("couldn't set %s/local-mac-address", path);
            exit(1);
        }
    }
}

/*
 * The device-tree edits the board's U-Boot makes before it starts
 * Linux, for a kernel QEMU loads itself. When the boot mode pins
 * select eSD, the U-Boot board code points the SD0 supplies at the
 * SD0 regulators. The Linux device trees for the board have no
 * "regulator-vcc-sdhi0" node, so vmmc-supply keeps its value; only
 * vqmmc-supply changes. U-Boot sets the MAC addresses of the Ethernet
 * channels. U-Boot's other edits are not made here.
 */
static void rzv2l_smarc_modify_dtb(const struct arm_boot_info *info,
                                   void *fdt)
{
    RZV2LSmarcState *s = container_of(info, RZV2LSmarcState, boot_info);

    rzv2l_smarc_add_mmc_aliases(fdt);
    rzv2l_smarc_set_mac_addresses(s, fdt);
    if (s->boot_mode == RZV2L_SMARC_BOOT_MODE_ESD) {
        rzv2l_smarc_set_supply(fdt, "/soc/mmc@11c00000", "vmmc-supply",
                               "/regulator-vcc-sdhi0");
        rzv2l_smarc_set_supply(fdt, "/soc/mmc@11c00000", "vqmmc-supply",
                               "/regulator-vccq-sdhi0");
    }
}

/*
 * The SD cards: -drive if=sd,index=0 goes into the slot on the module
 * (SDHI0), index=1 into the slot on the carrier board (SDHI1). A slot
 * without a drive is empty.
 */
static void rzv2l_smarc_attach_sd(RZV2LSmarcState *s)
{
    int i;

    for (i = 0; i < R9A07G054_NUM_SDHI; i++) {
        g_autofree char *name = g_strdup_printf("sd-bus%d", i);
        DriveInfo *di = drive_get(IF_SD, 0, i);
        DeviceState *card;

        if (!di) {
            continue;
        }
        card = qdev_new(TYPE_SD_CARD);
        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                                &error_fatal);
        qdev_realize_and_unref(card,
                               qdev_get_child_bus(DEVICE(&s->soc), name),
                               &error_fatal);
    }
}

/* The PHYs, and the NICs of the Ethernet channels */
static void rzv2l_smarc_init_eth(RZV2LSmarcState *s)
{
    int i;

    for (i = 0; i < R9A07G054_NUM_ETH; i++) {
        g_autofree char *name = g_strdup_printf("phy%d", i);
        DeviceState *eth = DEVICE(&s->soc.eth[i]);

        object_initialize_child(OBJECT(s), name, &s->phy[i],
                                TYPE_KSZ9131_PHY);
        sysbus_realize(SYS_BUS_DEVICE(&s->phy[i]), &error_fatal);
        object_property_set_link(OBJECT(eth), "phy", OBJECT(&s->phy[i]),
                                 &error_fatal);
        qdev_prop_set_uint8(eth, "phy-addr", RZV2L_SMARC_PHY_ADDR);
        qemu_configure_nic_device(eth, true, NULL);
    }
}

static void rzv2l_smarc_wire_eth(RZV2LSmarcState *s)
{
    int i;

    for (i = 0; i < R9A07G054_NUM_ETH; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->phy[i]), 0,
                           qdev_get_gpio_in_named(DEVICE(&s->soc.irqc), "irq",
                                                  rzv2l_smarc_phy_irq[i]));
        qdev_connect_gpio_out_named(DEVICE(&s->phy[i]), "link", 0,
                                    qdev_get_gpio_in_named(
                                        DEVICE(&s->soc.eth[i]),
                                        "phy-link", 0));
    }
}

static void rzv2l_smarc_init(MachineState *machine)
{
    RZV2LSmarcState *s = RZV2L_SMARC_MACHINE(machine);

    if (machine->ram_size != RZV2L_SMARC_RAM_SIZE) {
        error_report("RAM size 0x" RAM_ADDR_FMT " not supported: "
                     "the board has 2 GiB", machine->ram_size);
        exit(1);
    }

    s->boot_info = (struct arm_boot_info) {
        .loader_start = RZV2L_SMARC_LINUX_MEM_BASE,
        .board_id = -1,
        .ram_size = RZV2L_SMARC_LINUX_MEM_SIZE,
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
        .modify_dtb = rzv2l_smarc_modify_dtb,
    };

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_R9A07G054);
    object_property_set_uint(OBJECT(&s->soc), "boot-mode", s->boot_mode,
                             &error_fatal);
    object_property_set_bool(OBJECT(&s->soc), "debugen", s->debugen,
                             &error_fatal);
    /*
     * Without firmware, SCIF0 starts as the firmware would have left
     * the console, and the CPG and PFC with the clocks, resets and pin
     * functions the firmware would have set up.
     */
    object_property_set_bool(OBJECT(&s->soc), "scif0-direct-boot",
                             machine->kernel_filename != NULL, &error_fatal);
    object_property_set_bool(OBJECT(&s->soc), "cpg-direct-boot",
                             machine->kernel_filename != NULL, &error_fatal);
    object_property_set_bool(OBJECT(&s->soc), "pfc-direct-boot",
                             machine->kernel_filename != NULL, &error_fatal);
    rzv2l_smarc_init_eth(s);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(&s->soc), &error_fatal);
    rzv2l_smarc_wire_eth(s);
    rzv2l_smarc_attach_sd(s);

    memory_region_add_subregion(get_system_memory(), R9A07G054_DDR_BASE,
                                machine->ram);

    if (!qtest_enabled()) {
        arm_load_kernel(&s->soc.cpu[0], machine, &s->boot_info);
    }
}

/* Getter and setter: the visitor reads or writes boot_mode */
static void rzv2l_smarc_visit_boot_mode(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    RZV2LSmarcState *s = RZV2L_SMARC_MACHINE(obj);

    visit_type_uint8(v, name, &s->boot_mode, errp);
}

static bool rzv2l_smarc_get_debugen(Object *obj, Error **errp)
{
    return RZV2L_SMARC_MACHINE(obj)->debugen;
}

static void rzv2l_smarc_set_debugen(Object *obj, bool value, Error **errp)
{
    RZV2L_SMARC_MACHINE(obj)->debugen = value;
}

static void rzv2l_smarc_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a55"),
        NULL
    };

    mc->desc = "Renesas RZ/V2L SMARC EVK (2x Cortex-A55)";
    mc->init = rzv2l_smarc_init;
    mc->min_cpus = R9A07G054_NUM_CPUS;
    mc->max_cpus = R9A07G054_NUM_CPUS;
    mc->default_cpus = R9A07G054_NUM_CPUS;
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_id = "rzv2l-smarc.ram";
    mc->default_ram_size = RZV2L_SMARC_RAM_SIZE;
    mc->block_default_type = IF_SD;

    object_class_property_add(oc, "boot-mode", "uint8",
                              rzv2l_smarc_visit_boot_mode,
                              rzv2l_smarc_visit_boot_mode, NULL, NULL);
    object_class_property_set_description(oc, "boot-mode",
        "Boot mode pins MD_BOOT[2:0], 0-5 (default 0, eSD)");
    object_class_property_add_bool(oc, "debugen", rzv2l_smarc_get_debugen,
                                   rzv2l_smarc_set_debugen);
    object_class_property_set_description(oc, "debugen",
        "DEBUGEN pin (default off)");
}

static const TypeInfo rzv2l_smarc_machine_types[] = {
    {
        .name = TYPE_RZV2L_SMARC_MACHINE,
        .parent = TYPE_MACHINE,
        .class_init = rzv2l_smarc_machine_class_init,
        .instance_size = sizeof(RZV2LSmarcState),
        .interfaces = aarch64_machine_interfaces,
    },
};

DEFINE_TYPES(rzv2l_smarc_machine_types)
