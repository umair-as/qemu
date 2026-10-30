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
 * With -bios, the image is the board's TF-A BL31 and runs at EL3 as the
 * PSCI firmware. The machine does what BL2 does before it: it loads BL31,
 * the kernel (as BL33) and its device tree, writes BL2's parameter block
 * for BL31 into SRAM, and starts CPU0 in a stub that enters BL31 as BL2
 * does. The boot ROM, BL2 and DDR initialisation are not run.
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
#include "hw/core/loader.h"
#include "qemu/datadir.h"
#include "system/reset.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include <libfdt.h>

#define TYPE_RZV2L_SMARC_MACHINE MACHINE_TYPE_NAME("rzv2l-smarc")
OBJECT_DECLARE_SIMPLE_TYPE(RZV2LSmarcState, RZV2L_SMARC_MACHINE)

#define RZV2L_SMARC_RAM_SIZE        (2 * GiB)

/* The DDR window TF-A and U-Boot hand to Linux */
#define RZV2L_SMARC_LINUX_MEM_BASE  0x48000000
#define RZV2L_SMARC_LINUX_MEM_SIZE  0x78000000

/*
 * Firmware mode: where TF-A's BL2 for the board puts each image, and
 * where it leaves BL31's parameters (TF-A plat/renesas/rz/common/include:
 * BL31_BASE, BL31_LIMIT, PARAMS_BASE, BL2_BASE). The kernel and its DTB
 * go where U-Boot puts them on the board.
 */
#define RZV2L_SMARC_BL2_BASE        0x00012000
#define RZV2L_SMARC_BL31_PARAMS     0x0002f000
#define RZV2L_SMARC_BL31_BASE       0x44000000
#define RZV2L_SMARC_BL31_SIZE       0x00040000
#define RZV2L_SMARC_BL33_KERNEL     0x48200000
#define RZV2L_SMARC_BL33_DTB        0x58000000
#define RZV2L_SMARC_BL33_DTB_SIZE   (2 * MiB)

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

    rzv2l_smarc_add_mmc_aliases(fdt);
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

/*
 * The start of BL2's work that BL31 relies on: BL2 enters BL31 at EL3
 * with x0 pointing at its parameter block and x1-x3 zero (TF-A
 * plat/renesas/rz/common/bl2_plat_mem_params_desc.c, the BL31 entry
 * point). CPU0 starts here instead of in the boot ROM.
 */
static const uint32_t rzv2l_smarc_bl2_stub[] = {
    0x580000c0,     /* ldr  x0, params */
    0xd2800001,     /* mov  x1, #0 */
    0xd2800002,     /* mov  x2, #0 */
    0xd2800003,     /* mov  x3, #0 */
    0x58000084,     /* ldr  x4, bl31 */
    0xd61f0080,     /* br   x4 */
    RZV2L_SMARC_BL31_PARAMS, 0,         /* params: .quad */
    RZV2L_SMARC_BL31_BASE, 0,           /* bl31: .quad */
};

/*
 * BL2's parameter block for BL31, bl2_to_bl31_params_mem_t (TF-A
 * plat/renesas/rz/common/include/rz_private.h): the boot kind, then the
 * BL32 and BL33 entry points. BL2 zeroes it, so the boot kind is a cold
 * boot, and fills in the entry point of each image it loaded
 * (plat/renesas/rz/common/bl2_plat_setup.c,
 * bl2_plat_handle_post_image_load()). Only BL33 is given, so BL31 starts
 * no BL32. BL2 gives
 * U-Boot the address of its own device tree in x1; for a Linux BL33 the
 * device tree goes in x0, as the arm64 boot protocol requires.
 */
#define BL31_PARAMS_SIZE        184
#define BL31_PARAMS_BL33_EP     96
#define EP_PC                   8
#define EP_SPSR                 16
#define EP_ARGS                 24
#define PARAM_EP                1
#define VERSION_2               2
#define EP_INFO_SIZE            88
#define EP_NON_SECURE_EXEC      0x9         /* NON_SECURE | EXECUTABLE */
#define SPSR_EL1H_DAIF          0x3c5       /* EL1h, DAIF masked */

static void rzv2l_smarc_write_bl31_params(hwaddr kernel, hwaddr dtb)
{
    uint8_t params[BL31_PARAMS_SIZE] = { 0 };
    uint8_t *ep = params + BL31_PARAMS_BL33_EP;

    ep[0] = PARAM_EP;
    ep[1] = VERSION_2;
    stw_le_p(ep + 2, EP_INFO_SIZE);
    stl_le_p(ep + 4, EP_NON_SECURE_EXEC);
    stq_le_p(ep + EP_PC, kernel);
    stl_le_p(ep + EP_SPSR, SPSR_EL1H_DAIF);
    stq_le_p(ep + EP_ARGS, dtb);
    rom_add_blob_fixed("bl31-params", params, sizeof(params),
                       RZV2L_SMARC_BL31_PARAMS);
}

/* CPU objects are not reset with the machine; CPU0 restarts in the stub */
static void rzv2l_smarc_firmware_cpu_reset(void *opaque)
{
    CPUState *cs = opaque;

    cpu_reset(cs);
    if (cs->cpu_index == 0) {
        cpu_set_pc(cs, RZV2L_SMARC_BL2_BASE);
    }
}

/* Load a file at a fixed address, or exit with an error */
static void rzv2l_smarc_load(const char *what, const char *filename,
                             hwaddr addr, hwaddr max)
{
    if (load_image_targphys(filename, addr, max, NULL) < 0) {
        error_report("couldn't load %s '%s'", what, filename);
        exit(1);
    }
}

static void rzv2l_smarc_firmware_boot(RZV2LSmarcState *s,
                                      MachineState *machine)
{
    g_autofree char *bl31 = NULL;
    uint8_t header[64];
    CPUState *cs;
    int fd;

    if (!machine->kernel_filename || !machine->dtb) {
        error_report("-bios (TF-A BL31) needs -kernel and -dtb: the kernel "
                     "is BL31's next image");
        exit(1);
    }
    if (machine->initrd_filename) {
        error_report("-initrd is not supported with -bios");
        exit(1);
    }
    bl31 = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!bl31) {
        error_report("couldn't find BL31 image '%s'", machine->firmware);
        exit(1);
    }

    /* An arm64 Image with text_offset 0: placed at a 2 MiB boundary */
    fd = open(machine->kernel_filename, O_RDONLY | O_BINARY);
    if (fd < 0 || read(fd, header, sizeof(header)) != sizeof(header) ||
        ldl_le_p(header + 56) != 0x644d5241 || ldq_le_p(header + 8) != 0) {
        error_report("-kernel '%s' is not an arm64 Image with text_offset 0",
                     machine->kernel_filename);
        exit(1);
    }
    close(fd);

    rzv2l_smarc_load("BL31", bl31, RZV2L_SMARC_BL31_BASE,
                     RZV2L_SMARC_BL31_SIZE);
    rzv2l_smarc_load("kernel", machine->kernel_filename,
                     RZV2L_SMARC_BL33_KERNEL,
                     RZV2L_SMARC_BL33_DTB - RZV2L_SMARC_BL33_KERNEL);
    s->boot_info.dtb_filename = machine->dtb;
    if (arm_load_dtb(RZV2L_SMARC_BL33_DTB, &s->boot_info,
                     RZV2L_SMARC_BL33_DTB + RZV2L_SMARC_BL33_DTB_SIZE,
                     &address_space_memory, machine, &s->soc.cpu[0]) < 0) {
        exit(1);
    }
    rzv2l_smarc_write_bl31_params(RZV2L_SMARC_BL33_KERNEL,
                                  RZV2L_SMARC_BL33_DTB);
    rom_add_blob_fixed("bl2-stub", rzv2l_smarc_bl2_stub,
                       sizeof(rzv2l_smarc_bl2_stub), RZV2L_SMARC_BL2_BASE);

    CPU_FOREACH(cs) {
        qemu_register_reset(rzv2l_smarc_firmware_cpu_reset, cs);
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

    if (qtest_enabled()) {
        return;
    }
    if (machine->firmware) {
        rzv2l_smarc_firmware_boot(s, machine);
    } else {
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
