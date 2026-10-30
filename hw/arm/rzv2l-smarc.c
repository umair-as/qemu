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
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/arm/boot.h"
#include "hw/arm/r9a07g054.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/sd.h"
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

/* MD_BOOT[2:0] = 0: boot from the SD card on the module (eSD) */
#define RZV2L_SMARC_BOOT_MODE_ESD   0

struct RZV2LSmarcState {
    MachineState parent_obj;

    R9A07G054State soc;
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
 * The device-tree edits the board's U-Boot makes before it starts
 * Linux, for a kernel QEMU loads itself. When the boot mode pins
 * select eSD, the U-Boot board code points the SD0 supplies at the
 * SD0 regulators. The Linux device trees for the board have no
 * "regulator-vcc-sdhi0" node, so vmmc-supply keeps its value; only
 * vqmmc-supply changes. U-Boot's other edits are not made here.
 */
static void rzv2l_smarc_modify_dtb(const struct arm_boot_info *info,
                                   void *fdt)
{
    RZV2LSmarcState *s = container_of(info, RZV2LSmarcState, boot_info);

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
    sysbus_realize_and_unref(SYS_BUS_DEVICE(&s->soc), &error_fatal);
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
