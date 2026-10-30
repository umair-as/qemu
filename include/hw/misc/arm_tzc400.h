/*
 * Arm CoreLink TZC-400 TrustZone Address Space Controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * QEMU interface:
 *  + QOM property "num-filters": number of filter units (1-4)
 *  + QOM property "addr-width": width of the addresses it filters (bits)
 *  + sysbus MMIO region 0: the programming registers
 */

#ifndef HW_MISC_ARM_TZC400_H
#define HW_MISC_ARM_TZC400_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ARM_TZC400 "arm-tzc400"
OBJECT_DECLARE_SIMPLE_TYPE(ArmTZC400State, ARM_TZC400)

#define ARM_TZC400_MMIO_SIZE    0x1000
#define ARM_TZC400_NUM_REGIONS  9
/* base low, base high, top low, top high, attributes, ID access */
#define ARM_TZC400_REGION_REGS  6

struct ArmTZC400State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    uint32_t action;
    uint32_t gate_keeper;
    uint32_t speculation_ctrl;
    uint32_t region[ARM_TZC400_NUM_REGIONS][ARM_TZC400_REGION_REGS];

    /* Properties */
    uint8_t num_filters;
    uint8_t addr_width;
};

#endif
