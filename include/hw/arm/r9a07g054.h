/*
 * Renesas RZ/V2L (R9A07G054) SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * QEMU interface:
 *  + QOM property "boot-mode": alias of the SYSC "boot-mode" property
 *  + QOM property "debugen": alias of the SYSC "debugen" property
 *  + QOM property "scif0-direct-boot": alias of the SCIF0 "direct-boot"
 *    property; the board sets it when it loads a kernel directly
 *  + the SoC maps its devices into the system address space at realize
 */

#ifndef HW_ARM_R9A07G054_H
#define HW_ARM_R9A07G054_H

#include "target/arm/cpu.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/renesas_rzg2l_sysc.h"
#include "hw/char/renesas_rz_scifa.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_R9A07G054 "r9a07g054"
OBJECT_DECLARE_SIMPLE_TYPE(R9A07G054State, R9A07G054)

#define R9A07G054_DDR_BASE      0x40000000

enum {
    R9A07G054_NUM_CPUS  = 2,
    /* Shared peripheral interrupts, INTID 32-511 */
    R9A07G054_NUM_SPIS  = 480,
    R9A07G054_NUM_SCIF  = 5,
};

struct R9A07G054State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    ARMCPU cpu[R9A07G054_NUM_CPUS];
    GICv3State gic;
    RenesasRZG2LSysCState sysc;
    RenesasRZSCIFAState scif[R9A07G054_NUM_SCIF];
};

#endif
