/*
 * Renesas RZ/V2L DDR memory controller and PHY (stub)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * A register-level stub of the DDR4 memory controller and its PHY in the
 * Renesas RZ/V2L, enough for the boot firmware's DDR set-up and training
 * to complete. The RZ/V2L Group User's Manual: Hardware (R01UH0936EJ)
 * documents neither block's registers (section 9); what the stub stores
 * and computes is what TF-A's BL2 waits for.
 *
 * QEMU interface:
 *  + sysbus MMIO region 0: the memory controller's registers (64 KiB)
 *  + sysbus MMIO region 1: the PHY's registers (64 KiB)
 */

#ifndef HW_MISC_RENESAS_RZV2L_DDR_H
#define HW_MISC_RENESAS_RZV2L_DDR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RZV2L_DDR "renesas-rzv2l-ddr"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZV2LDDRState, RENESAS_RZV2L_DDR)

#define RENESAS_RZV2L_DDR_MMIO_SIZE    0x10000
/* Controller registers 0x000-0x7f8, PHY registers 0x000-0x264 */
#define RENESAS_RZV2L_DDR_MC_REGS      0x1ff
#define RENESAS_RZV2L_DDR_PHY_REGS     0x9a

struct RenesasRZV2LDDRState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mc_iomem;
    MemoryRegion phy_iomem;

    uint32_t mc[RENESAS_RZV2L_DDR_MC_REGS];
    uint32_t phy[RENESAS_RZV2L_DDR_PHY_REGS];
    bool init_done;
    bool mrw_done;
    uint8_t lp_state;
};

#endif
