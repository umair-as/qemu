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
#include "system/qtest.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"

#define TYPE_RZV2L_SMARC_MACHINE MACHINE_TYPE_NAME("rzv2l-smarc")
OBJECT_DECLARE_SIMPLE_TYPE(RZV2LSmarcState, RZV2L_SMARC_MACHINE)

#define RZV2L_SMARC_RAM_SIZE        (2 * GiB)

/* The DDR window TF-A and U-Boot hand to Linux */
#define RZV2L_SMARC_LINUX_MEM_BASE  0x48000000
#define RZV2L_SMARC_LINUX_MEM_SIZE  0x78000000

struct RZV2LSmarcState {
    MachineState parent_obj;

    R9A07G054State soc;
    struct arm_boot_info boot_info;

    /* Board switches, latched by the SoC at power-on */
    uint8_t boot_mode;
    bool debugen;
};

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
    };

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_R9A07G054);
    object_property_set_uint(OBJECT(&s->soc), "boot-mode", s->boot_mode,
                             &error_fatal);
    object_property_set_bool(OBJECT(&s->soc), "debugen", s->debugen,
                             &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(&s->soc), &error_fatal);

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
