/*
 * Renesas RZ/G2L family System Controller (SYSC)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Model of the identification registers, the Cortex-A55 reset vectors
 * and the Cortex-A55 sleep handshake of the System Controller found in
 * the Renesas RZ/G2L family (RZ/G2L, RZ/V2L), documented in the RZ/V2L
 * Group User's Manual: Hardware (R01UH0936EJ), section 6.
 *
 * QEMU interface:
 *  + QOM property "boot-mode": latched MD_BOOT[2:0] pins (0-5)
 *  + QOM property "debugen": latched DEBUGEN pin
 *  + QOM property "md-clks": latched MD_CLKS pin
 *  + QOM property "md-oscdrv": latched MD_OSCDRV[1:0] pins (0-3)
 *  + QOM property "sec-en": the part has the security function
 *    (SYS_LSI_MODE.STAT_SEC_EN; differs between parts of the family)
 *  + QOM property "lsi-devid": value of SYS_LSI_DEVID (must be set)
 *  + QOM property "lsi-prr": value of SYS_LSI_PRR
 *  + sysbus MMIO region 0: the SYSC register bank
 *  + rzg2l_sysc_reset_vector(): the address Cortex-A55 core n starts
 *    from when it is released from reset
 */

#ifndef HW_MISC_RENESAS_RZG2L_SYSC_H
#define HW_MISC_RENESAS_RZG2L_SYSC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RZG2L_SYSC "renesas-rzg2l-sysc"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZG2LSysCState, RENESAS_RZG2L_SYSC)

#define RENESAS_RZG2L_SYSC_MMIO_SIZE 0x10000
#define RENESAS_RZG2L_SYSC_NUM_CA55  2

struct RenesasRZG2LSysCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    /* Properties */
    uint8_t boot_mode;
    bool debugen;
    bool md_clks;
    uint8_t md_oscdrv;
    bool sec_en;
    uint32_t lsi_devid;
    uint32_t lsi_prr;

    /* Registers */
    uint64_t rvba[RENESAS_RZG2L_SYSC_NUM_CA55];
    uint32_t lp_ctl1;
    uint32_t lp_ctl2;
    uint32_t lp_ctl5;
    uint32_t lp_ctl6;
};

uint64_t rzg2l_sysc_reset_vector(RenesasRZG2LSysCState *s, int core);

#endif
