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
 *  + QOM property "cpg-direct-boot": alias of the CPG "direct-boot"
 *    property; the board sets it when it loads a kernel directly
 *  + QOM property "pfc-direct-boot": alias of the PFC "direct-boot"
 *    property; the board sets it when it loads a kernel directly
 *  + child buses "sd-bus0", "sd-bus1": the cards of SDHI0 and SDHI1
 *  + the SoC maps its devices into the system address space at realize
 */

#ifndef HW_ARM_R9A07G054_H
#define HW_ARM_R9A07G054_H

#include "target/arm/cpu.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/renesas_rzg2l_sysc.h"
#include "hw/misc/renesas_rzv2l_cpg.h"
#include "hw/char/renesas_rz_scifa.h"
#include "hw/timer/renesas_rz_ostm.h"
#include "hw/timer/coresight_tsgen.h"
#include "hw/misc/arm_tzc400.h"
#include "hw/watchdog/renesas_rzg2l_wdt.h"
#include "hw/intc/renesas_rzg2l_irqc.h"
#include "hw/gpio/renesas_rzg2l_pfc.h"
#include "hw/sd/renesas_sdhi.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_R9A07G054 "r9a07g054"
OBJECT_DECLARE_SIMPLE_TYPE(R9A07G054State, R9A07G054)

#define R9A07G054_DDR_BASE      0x40000000

/* On-chip SRAM for the Cortex-M33 (MCPU) and the Cortex-A55 (ACPU) */
#define R9A07G054_SRAM_MCPU_BASE    0x00010000
#define R9A07G054_SRAM_ACPU_BASE    0x00020000
#define R9A07G054_SRAM_SIZE         (64 * KiB)

enum {
    R9A07G054_NUM_CPUS  = 2,
    /* Shared peripheral interrupts, INTID 32-511 */
    R9A07G054_NUM_SPIS  = 480,
    R9A07G054_NUM_SCIF  = 5,
    R9A07G054_NUM_OSTM  = 3,
    /* WDT0, WDT1: Cortex-A55 cores 0 and 1; WDT2: Cortex-M33 */
    R9A07G054_NUM_WDT   = 3,
    R9A07G054_NUM_SDHI  = 2,
    /* TZC0-3: SRAM ACPU, SRAM MCPU, SPI Multi I/O, DDR */
    R9A07G054_NUM_TZC   = 4,
};

struct R9A07G054State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    ARMCPU cpu[R9A07G054_NUM_CPUS];
    GICv3State gic;
    RenesasRZG2LSysCState sysc;
    CoreSightTSGenState syc;
    ArmTZC400State tzc[R9A07G054_NUM_TZC];
    RenesasRZV2LCPGState cpg;
    RenesasRZSCIFAState scif[R9A07G054_NUM_SCIF];
    RenesasRZOSTMState ostm[R9A07G054_NUM_OSTM];
    RenesasRZG2LWDTState wdt[R9A07G054_NUM_WDT];
    RenesasSDHIState sdhi[R9A07G054_NUM_SDHI];
    RenesasRZG2LIRQCState irqc;
    RenesasRZG2LPFCState pfc;
    MemoryRegion sram_mcpu;
    MemoryRegion sram_acpu;
};

#endif
