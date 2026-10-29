/*
 * Renesas RZ/G2L family pin function controller (PFC/GPIO)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The 592 registers of manual section 41.2: per port Pn, PMn, PMCn,
 * PFCm, PINn, ISEL, IOLH, SR, PUPD, IEN and the noise filter registers,
 * and the voltage, write-protect and Ethernet mode registers. Reset
 * values and implemented bits come from the register tables and the
 * pin correspondence tables (Tables 41.1-41.13).
 *
 * - An access is split into bytes. A byte of a register follows that
 *   register's implemented bits; a byte with no register reads 0 and
 *   ignores writes, as an EVK read of an undefined offset shows. Such
 *   bytes, and access sizes the manual does not list for a register,
 *   are logged.
 * - PWPR (section 41.3.10): the PFC registers take writes only while
 *   PFCWE (bit 6) is 1, and PFCWE changes only while B0WI (bit 7) was 0
 *   before the write. The manual gives PWPR as 8-bit; Linux and TF-A
 *   write it with 32-bit stores, so the model accepts both.
 * - PINn reads the output data Pn for pins in output mode with input
 *   enabled (PMn = 11b) and 0 for the others: no pin is driven from
 *   outside.
 *
 * Not modelled: the electrical settings (drive, slew, pull, noise
 * filters, IO voltage) and the GPIO interrupt routing (ISEL) are only
 * stored; the boot-mode pins do not make SD_ch0 or QSPI read-only; pin
 * functions do not connect or disconnect peripherals.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/gpio/renesas_rzg2l_pfc.h"

enum {
    PFC_P, PFC_PM, PFC_PMC, PFC_PFC, PFC_PIN, PFC_ISEL, PFC_IOLH, PFC_SR,
    PFC_IEN, PFC_PUPD, PFC_FILON, PFC_FILNUM, PFC_FILCLK,
    PFC_SD_CH0, PFC_SD_CH1, PFC_QSPI, PFC_ETH_CH0, PFC_ETH_CH1, PFC_PWPR,
    PFC_ETH_MODE,
};

#define PFC_REG_SPACE       0x3020

#define A_PWPR              0x3014
#define PWPR_B0WI           0x80
#define PWPR_PFCWE          0x40

/* Register families by port index off: offset = base + stride * off */
#define P_BASE              0x0000
#define PM_BASE             0x0100
#define PIN_BASE            0x0800

typedef struct {
    uint16_t offset;
    uint8_t family;
    uint8_t width;          /* bytes */
    uint8_t sizes;          /* access sizes the manual lists: 1, 2, 4 */
    uint32_t reset;
    uint32_t mask;          /* implemented bits */
    bool ro;
} PFCReg;

static const PFCReg pfc_regs[] = {
    { 0x0010, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P10 */
    { 0x0011, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P11 */
    { 0x0012, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P12 */
    { 0x0013, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P13 */
    { 0x0014, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P14 */
    { 0x0015, PFC_P, 1, 0x1, 0x0, 0x7, false }, /* P15 */
    { 0x0016, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P16 */
    { 0x0017, PFC_P, 1, 0x1, 0x0, 0x7, false }, /* P17 */
    { 0x0018, PFC_P, 1, 0x1, 0x0, 0x7, false }, /* P18 */
    { 0x0019, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P19 */
    { 0x001a, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P1A */
    { 0x001b, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P1B */
    { 0x001c, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P1C */
    { 0x001d, PFC_P, 1, 0x1, 0x0, 0x7, false }, /* P1D */
    { 0x001e, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P1E */
    { 0x001f, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P1F */
    { 0x0020, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P20 */
    { 0x0021, PFC_P, 1, 0x1, 0x0, 0x7, false }, /* P21 */
    { 0x0022, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P22 */
    { 0x0023, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P23 */
    { 0x0024, PFC_P, 1, 0x1, 0x0, 0x7, false }, /* P24 */
    { 0x0025, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P25 */
    { 0x0026, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P26 */
    { 0x0027, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P27 */
    { 0x0028, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P28 */
    { 0x0029, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P29 */
    { 0x002a, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P2A */
    { 0x002b, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P2B */
    { 0x002c, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P2C */
    { 0x002d, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P2D */
    { 0x002e, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P2E */
    { 0x002f, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P2F */
    { 0x0030, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P30 */
    { 0x0031, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P31 */
    { 0x0032, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P32 */
    { 0x0033, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P33 */
    { 0x0034, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P34 */
    { 0x0035, PFC_P, 1, 0x1, 0x0, 0x7, false }, /* P35 */
    { 0x0036, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P36 */
    { 0x0037, PFC_P, 1, 0x1, 0x0, 0x7, false }, /* P37 */
    { 0x0038, PFC_P, 1, 0x1, 0x0, 0x7, false }, /* P38 */
    { 0x0039, PFC_P, 1, 0x1, 0x0, 0x3, false }, /* P39 */
    { 0x003a, PFC_P, 1, 0x1, 0x0, 0x1f, false }, /* P3A */
    { 0x003b, PFC_P, 1, 0x1, 0x0, 0xf, false }, /* P3B */
    { 0x003c, PFC_P, 1, 0x1, 0x0, 0xf, false }, /* P3C */
    { 0x003d, PFC_P, 1, 0x1, 0x0, 0xf, false }, /* P3D */
    { 0x003e, PFC_P, 1, 0x1, 0x0, 0xf, false }, /* P3E */
    { 0x003f, PFC_P, 1, 0x1, 0x0, 0xf, false }, /* P3F */
    { 0x0040, PFC_P, 1, 0x1, 0x0, 0x1f, false }, /* P40 */
    { 0x0120, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM10 */
    { 0x0122, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM11 */
    { 0x0124, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM12 */
    { 0x0126, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM13 */
    { 0x0128, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM14 */
    { 0x012a, PFC_PM, 2, 0x3, 0x0, 0x3f, false }, /* PM15 */
    { 0x012c, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM16 */
    { 0x012e, PFC_PM, 2, 0x3, 0x0, 0x3f, false }, /* PM17 */
    { 0x0130, PFC_PM, 2, 0x3, 0x0, 0x3f, false }, /* PM18 */
    { 0x0132, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM19 */
    { 0x0134, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM1A */
    { 0x0136, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM1B */
    { 0x0138, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM1C */
    { 0x013a, PFC_PM, 2, 0x3, 0x0, 0x3f, false }, /* PM1D */
    { 0x013c, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM1E */
    { 0x013e, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM1F */
    { 0x0140, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM20 */
    { 0x0142, PFC_PM, 2, 0x3, 0x0, 0x3f, false }, /* PM21 */
    { 0x0144, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM22 */
    { 0x0146, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM23 */
    { 0x0148, PFC_PM, 2, 0x3, 0x0, 0x3f, false }, /* PM24 */
    { 0x014a, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM25 */
    { 0x014c, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM26 */
    { 0x014e, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM27 */
    { 0x0150, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM28 */
    { 0x0152, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM29 */
    { 0x0154, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM2A */
    { 0x0156, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM2B */
    { 0x0158, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM2C */
    { 0x015a, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM2D */
    { 0x015c, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM2E */
    { 0x015e, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM2F */
    { 0x0160, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM30 */
    { 0x0162, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM31 */
    { 0x0164, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM32 */
    { 0x0166, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM33 */
    { 0x0168, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM34 */
    { 0x016a, PFC_PM, 2, 0x3, 0x0, 0x3f, false }, /* PM35 */
    { 0x016c, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM36 */
    { 0x016e, PFC_PM, 2, 0x3, 0x0, 0x3f, false }, /* PM37 */
    { 0x0170, PFC_PM, 2, 0x3, 0x0, 0x3f, false }, /* PM38 */
    { 0x0172, PFC_PM, 2, 0x3, 0x0, 0xf, false }, /* PM39 */
    { 0x0174, PFC_PM, 2, 0x3, 0x0, 0x3ff, false }, /* PM3A */
    { 0x0176, PFC_PM, 2, 0x3, 0x0, 0xff, false }, /* PM3B */
    { 0x0178, PFC_PM, 2, 0x3, 0x0, 0xff, false }, /* PM3C */
    { 0x017a, PFC_PM, 2, 0x3, 0x0, 0xff, false }, /* PM3D */
    { 0x017c, PFC_PM, 2, 0x3, 0x0, 0xff, false }, /* PM3E */
    { 0x017e, PFC_PM, 2, 0x3, 0x0, 0xff, false }, /* PM3F */
    { 0x0180, PFC_PM, 2, 0x3, 0x0, 0x3ff, false }, /* PM40 */
    { 0x0210, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC10 */
    { 0x0211, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC11 */
    { 0x0212, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC12 */
    { 0x0213, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC13 */
    { 0x0214, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC14 */
    { 0x0215, PFC_PMC, 1, 0x1, 0x0, 0x7, false }, /* PMC15 */
    { 0x0216, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC16 */
    { 0x0217, PFC_PMC, 1, 0x1, 0x0, 0x7, false }, /* PMC17 */
    { 0x0218, PFC_PMC, 1, 0x1, 0x0, 0x7, false }, /* PMC18 */
    { 0x0219, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC19 */
    { 0x021a, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC1A */
    { 0x021b, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC1B */
    { 0x021c, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC1C */
    { 0x021d, PFC_PMC, 1, 0x1, 0x0, 0x7, false }, /* PMC1D */
    { 0x021e, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC1E */
    { 0x021f, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC1F */
    { 0x0220, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC20 */
    { 0x0221, PFC_PMC, 1, 0x1, 0x0, 0x7, false }, /* PMC21 */
    { 0x0222, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC22 */
    { 0x0223, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC23 */
    { 0x0224, PFC_PMC, 1, 0x1, 0x0, 0x7, false }, /* PMC24 */
    { 0x0225, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC25 */
    { 0x0226, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC26 */
    { 0x0227, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC27 */
    { 0x0228, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC28 */
    { 0x0229, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC29 */
    { 0x022a, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC2A */
    { 0x022b, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC2B */
    { 0x022c, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC2C */
    { 0x022d, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC2D */
    { 0x022e, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC2E */
    { 0x022f, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC2F */
    { 0x0230, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC30 */
    { 0x0231, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC31 */
    { 0x0232, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC32 */
    { 0x0233, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC33 */
    { 0x0234, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC34 */
    { 0x0235, PFC_PMC, 1, 0x1, 0x0, 0x7, false }, /* PMC35 */
    { 0x0236, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC36 */
    { 0x0237, PFC_PMC, 1, 0x1, 0x0, 0x7, false }, /* PMC37 */
    { 0x0238, PFC_PMC, 1, 0x1, 0x0, 0x7, false }, /* PMC38 */
    { 0x0239, PFC_PMC, 1, 0x1, 0x0, 0x3, false }, /* PMC39 */
    { 0x023a, PFC_PMC, 1, 0x1, 0x0, 0x1f, false }, /* PMC3A */
    { 0x023b, PFC_PMC, 1, 0x1, 0x0, 0xf, false }, /* PMC3B */
    { 0x023c, PFC_PMC, 1, 0x1, 0x0, 0xf, false }, /* PMC3C */
    { 0x023d, PFC_PMC, 1, 0x1, 0x0, 0xf, false }, /* PMC3D */
    { 0x023e, PFC_PMC, 1, 0x1, 0x0, 0xf, false }, /* PMC3E */
    { 0x023f, PFC_PMC, 1, 0x1, 0x0, 0xf, false }, /* PMC3F */
    { 0x0240, PFC_PMC, 1, 0x1, 0x0, 0x1f, false }, /* PMC40 */
    { 0x0440, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC10 */
    { 0x0444, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC11 */
    { 0x0448, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC12 */
    { 0x044c, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC13 */
    { 0x0450, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC14 */
    { 0x0454, PFC_PFC, 4, 0x7, 0x0, 0x777, false }, /* PFC15 */
    { 0x0458, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC16 */
    { 0x045c, PFC_PFC, 4, 0x7, 0x0, 0x777, false }, /* PFC17 */
    { 0x0460, PFC_PFC, 4, 0x7, 0x0, 0x777, false }, /* PFC18 */
    { 0x0464, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC19 */
    { 0x0468, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC1A */
    { 0x046c, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC1B */
    { 0x0470, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC1C */
    { 0x0474, PFC_PFC, 4, 0x7, 0x0, 0x777, false }, /* PFC1D */
    { 0x0478, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC1E */
    { 0x047c, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC1F */
    { 0x0480, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC20 */
    { 0x0484, PFC_PFC, 4, 0x7, 0x0, 0x777, false }, /* PFC21 */
    { 0x0488, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC22 */
    { 0x048c, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC23 */
    { 0x0490, PFC_PFC, 4, 0x7, 0x0, 0x777, false }, /* PFC24 */
    { 0x0494, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC25 */
    { 0x0498, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC26 */
    { 0x049c, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC27 */
    { 0x04a0, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC28 */
    { 0x04a4, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC29 */
    { 0x04a8, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC2A */
    { 0x04ac, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC2B */
    { 0x04b0, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC2C */
    { 0x04b4, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC2D */
    { 0x04b8, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC2E */
    { 0x04bc, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC2F */
    { 0x04c0, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC30 */
    { 0x04c4, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC31 */
    { 0x04c8, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC32 */
    { 0x04cc, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC33 */
    { 0x04d0, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC34 */
    { 0x04d4, PFC_PFC, 4, 0x7, 0x0, 0x777, false }, /* PFC35 */
    { 0x04d8, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC36 */
    { 0x04dc, PFC_PFC, 4, 0x7, 0x0, 0x777, false }, /* PFC37 */
    { 0x04e0, PFC_PFC, 4, 0x7, 0x0, 0x777, false }, /* PFC38 */
    { 0x04e4, PFC_PFC, 4, 0x7, 0x0, 0x77, false }, /* PFC39 */
    { 0x04e8, PFC_PFC, 4, 0x7, 0x0, 0x77777, false }, /* PFC3A */
    { 0x04ec, PFC_PFC, 4, 0x7, 0x0, 0x7777, false }, /* PFC3B */
    { 0x04f0, PFC_PFC, 4, 0x7, 0x0, 0x7777, false }, /* PFC3C */
    { 0x04f4, PFC_PFC, 4, 0x7, 0x0, 0x7777, false }, /* PFC3D */
    { 0x04f8, PFC_PFC, 4, 0x7, 0x0, 0x7777, false }, /* PFC3E */
    { 0x04fc, PFC_PFC, 4, 0x7, 0x0, 0x7777, false }, /* PFC3F */
    { 0x0500, PFC_PFC, 4, 0x7, 0x0, 0x77777, false }, /* PFC40 */
    { 0x0810, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN10 */
    { 0x0811, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN11 */
    { 0x0812, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN12 */
    { 0x0813, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN13 */
    { 0x0814, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN14 */
    { 0x0815, PFC_PIN, 1, 0x1, 0x0, 0x7, true }, /* PIN15 */
    { 0x0816, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN16 */
    { 0x0817, PFC_PIN, 1, 0x1, 0x0, 0x7, true }, /* PIN17 */
    { 0x0818, PFC_PIN, 1, 0x1, 0x0, 0x7, true }, /* PIN18 */
    { 0x0819, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN19 */
    { 0x081a, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN1A */
    { 0x081b, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN1B */
    { 0x081c, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN1C */
    { 0x081d, PFC_PIN, 1, 0x1, 0x0, 0x7, true }, /* PIN1D */
    { 0x081e, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN1E */
    { 0x081f, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN1F */
    { 0x0820, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN20 */
    { 0x0821, PFC_PIN, 1, 0x1, 0x0, 0x7, true }, /* PIN21 */
    { 0x0822, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN22 */
    { 0x0823, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN23 */
    { 0x0824, PFC_PIN, 1, 0x1, 0x0, 0x7, true }, /* PIN24 */
    { 0x0825, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN25 */
    { 0x0826, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN26 */
    { 0x0827, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN27 */
    { 0x0828, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN28 */
    { 0x0829, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN29 */
    { 0x082a, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN2A */
    { 0x082b, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN2B */
    { 0x082c, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN2C */
    { 0x082d, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN2D */
    { 0x082e, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN2E */
    { 0x082f, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN2F */
    { 0x0830, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN30 */
    { 0x0831, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN31 */
    { 0x0832, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN32 */
    { 0x0833, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN33 */
    { 0x0834, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN34 */
    { 0x0835, PFC_PIN, 1, 0x1, 0x0, 0x7, true }, /* PIN35 */
    { 0x0836, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN36 */
    { 0x0837, PFC_PIN, 1, 0x1, 0x0, 0x7, true }, /* PIN37 */
    { 0x0838, PFC_PIN, 1, 0x1, 0x0, 0x7, true }, /* PIN38 */
    { 0x0839, PFC_PIN, 1, 0x1, 0x0, 0x3, true }, /* PIN39 */
    { 0x083a, PFC_PIN, 1, 0x1, 0x0, 0x1f, true }, /* PIN3A */
    { 0x083b, PFC_PIN, 1, 0x1, 0x0, 0xf, true }, /* PIN3B */
    { 0x083c, PFC_PIN, 1, 0x1, 0x0, 0xf, true }, /* PIN3C */
    { 0x083d, PFC_PIN, 1, 0x1, 0x0, 0xf, true }, /* PIN3D */
    { 0x083e, PFC_PIN, 1, 0x1, 0x0, 0xf, true }, /* PIN3E */
    { 0x083f, PFC_PIN, 1, 0x1, 0x0, 0xf, true }, /* PIN3F */
    { 0x0840, PFC_PIN, 1, 0x1, 0x0, 0x1f, true }, /* PIN40 */
    { 0x1010, PFC_IOLH, 4, 0x7, 0x1, 0x3, false }, /* IOLH02 */
    { 0x1018, PFC_IOLH, 4, 0x7, 0x1, 0x3, false }, /* IOLH03 */
    { 0x1030, PFC_IOLH, 4, 0x7, 0x20202, 0x30303, false }, /* IOLH06 */
    { 0x1038, PFC_IOLH, 4, 0x7, 0x2020202, 0x3030303, false }, /* IOLH07_L */
    { 0x103c, PFC_IOLH, 4, 0x7, 0x2020202, 0x3030303, false }, /* IOLH07_H */
    { 0x1040, PFC_IOLH, 4, 0x7, 0x202, 0x303, false }, /* IOLH08 */
    { 0x1048, PFC_IOLH, 4, 0x7, 0x2020202, 0x3030303, false }, /* IOLH09 */
    { 0x1050, PFC_IOLH, 4, 0x7, 0x2020202, 0x3030303, false }, /* IOLH0A_L */
    { 0x1054, PFC_IOLH, 4, 0x7, 0x202, 0x303, false }, /* IOLH0A_H */
    { 0x1058, PFC_IOLH, 4, 0x7, 0x2020202, 0x3030303, false }, /* IOLH0B_L */
    { 0x105c, PFC_IOLH, 4, 0x7, 0x202, 0x303, false }, /* IOLH0B_H */
    { 0x1060, PFC_IOLH, 4, 0x7, 0x202, 0x303, false }, /* IOLH0C */
    { 0x1068, PFC_IOLH, 4, 0x7, 0x1, 0x3, false }, /* IOLH0D */
    { 0x1080, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH10 */
    { 0x1088, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH11 */
    { 0x1090, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH12 */
    { 0x1098, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH13 */
    { 0x10a0, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH14 */
    { 0x10a8, PFC_IOLH, 4, 0x7, 0x10101, 0x30303, false }, /* IOLH15 */
    { 0x10b0, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH16 */
    { 0x10b8, PFC_IOLH, 4, 0x7, 0x10101, 0x30303, false }, /* IOLH17 */
    { 0x10c0, PFC_IOLH, 4, 0x7, 0x10101, 0x30303, false }, /* IOLH18 */
    { 0x10c8, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH19 */
    { 0x10d0, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH1A */
    { 0x10d8, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH1B */
    { 0x10e0, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH1C */
    { 0x10e8, PFC_IOLH, 4, 0x7, 0x10101, 0x30303, false }, /* IOLH1D */
    { 0x10f0, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH1E */
    { 0x10f8, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH1F */
    { 0x1100, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH20 */
    { 0x1108, PFC_IOLH, 4, 0x7, 0x10101, 0x30303, false }, /* IOLH21 */
    { 0x1110, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH22 */
    { 0x1118, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH23 */
    { 0x11b0, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH36 */
    { 0x11b8, PFC_IOLH, 4, 0x7, 0x10101, 0x30303, false }, /* IOLH37 */
    { 0x11c0, PFC_IOLH, 4, 0x7, 0x10101, 0x30303, false }, /* IOLH38 */
    { 0x11c8, PFC_IOLH, 4, 0x7, 0x101, 0x303, false }, /* IOLH39 */
    { 0x11d0, PFC_IOLH, 4, 0x7, 0x1010101, 0x3030303, false }, /* IOLH3A_L */
    { 0x11d4, PFC_IOLH, 4, 0x7, 0x1, 0x3, false }, /* IOLH3A_H */
    { 0x11d8, PFC_IOLH, 4, 0x7, 0x1010101, 0x3030303, false }, /* IOLH3B */
    { 0x11e0, PFC_IOLH, 4, 0x7, 0x1010101, 0x3030303, false }, /* IOLH3C */
    { 0x11e8, PFC_IOLH, 4, 0x7, 0x1010101, 0x3030303, false }, /* IOLH3D */
    { 0x11f0, PFC_IOLH, 4, 0x7, 0x1010101, 0x3030303, false }, /* IOLH3E */
    { 0x11f8, PFC_IOLH, 4, 0x7, 0x1010101, 0x3030303, false }, /* IOLH3F */
    { 0x1200, PFC_IOLH, 4, 0x7, 0x1010101, 0x3030303, false }, /* IOLH40_L */
    { 0x1204, PFC_IOLH, 4, 0x7, 0x1, 0x3, false }, /* IOLH40_H */
    { 0x1410, PFC_SR, 4, 0x7, 0x1, 0x1, false }, /* SR02 */
    { 0x1418, PFC_SR, 4, 0x7, 0x1, 0x1, false }, /* SR03 */
    { 0x1430, PFC_SR, 4, 0x7, 0x10101, 0x10101, false }, /* SR06 */
    { 0x1438, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR07_L */
    { 0x143c, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR07_H */
    { 0x1440, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR08 */
    { 0x1448, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR09 */
    { 0x1450, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR0A_L */
    { 0x1454, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR0A_H */
    { 0x1458, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR0B_L */
    { 0x145c, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR0B_H */
    { 0x1460, PFC_SR, 4, 0x7, 0x10101, 0x10101, false }, /* SR0C */
    { 0x1468, PFC_SR, 4, 0x7, 0x1, 0x1, false }, /* SR0D */
    { 0x1480, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR10 */
    { 0x1488, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR11 */
    { 0x1490, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR12 */
    { 0x1498, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR13 */
    { 0x14a0, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR14 */
    { 0x14a8, PFC_SR, 4, 0x7, 0x10101, 0x10101, false }, /* SR15 */
    { 0x14b0, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR16 */
    { 0x14b8, PFC_SR, 4, 0x7, 0x10101, 0x10101, false }, /* SR17 */
    { 0x14c0, PFC_SR, 4, 0x7, 0x10101, 0x10101, false }, /* SR18 */
    { 0x14c8, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR19 */
    { 0x14d0, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR1A */
    { 0x14d8, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR1B */
    { 0x14e0, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR1C */
    { 0x14e8, PFC_SR, 4, 0x7, 0x10101, 0x10101, false }, /* SR1D */
    { 0x14f0, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR1E */
    { 0x14f8, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR1F */
    { 0x1500, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR20 */
    { 0x1508, PFC_SR, 4, 0x7, 0x10101, 0x10101, false }, /* SR21 */
    { 0x1510, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR22 */
    { 0x1518, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR23 */
    { 0x15b0, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR36 */
    { 0x15b8, PFC_SR, 4, 0x7, 0x10101, 0x10101, false }, /* SR37 */
    { 0x15c0, PFC_SR, 4, 0x7, 0x10101, 0x10101, false }, /* SR38 */
    { 0x15c8, PFC_SR, 4, 0x7, 0x101, 0x101, false }, /* SR39 */
    { 0x15d0, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR3A_L */
    { 0x15d4, PFC_SR, 4, 0x7, 0x1, 0x1, false }, /* SR3A_H */
    { 0x15d8, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR3B */
    { 0x15e0, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR3C */
    { 0x15e8, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR3D */
    { 0x15f0, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR3E */
    { 0x15f8, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR3F */
    { 0x1600, PFC_SR, 4, 0x7, 0x1010101, 0x1010101, false }, /* SR40_L */
    { 0x1604, PFC_SR, 4, 0x7, 0x1, 0x1, false }, /* SR40_H */
    { 0x1810, PFC_IEN, 4, 0x7, 0x0, 0x1, false }, /* IEN02 */
    { 0x1820, PFC_IEN, 4, 0x7, 0x0, 0x101, false }, /* IEN04 */
    { 0x1830, PFC_IEN, 4, 0x7, 0x0, 0x100, false }, /* IEN06 */
    { 0x1838, PFC_IEN, 4, 0x7, 0x0, 0x1010101, false }, /* IEN07_L */
    { 0x183c, PFC_IEN, 4, 0x7, 0x0, 0x1010101, false }, /* IEN07_H */
    { 0x1840, PFC_IEN, 4, 0x7, 0x0, 0x100, false }, /* IEN08 */
    { 0x1848, PFC_IEN, 4, 0x7, 0x0, 0x1010101, false }, /* IEN09 */
    { 0x1870, PFC_IEN, 4, 0x7, 0x0, 0x1010101, false }, /* IEN0E */
    { 0x1c80, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD10 */
    { 0x1c88, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD11 */
    { 0x1c90, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD12 */
    { 0x1c98, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD13 */
    { 0x1ca0, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD14 */
    { 0x1ca8, PFC_PUPD, 4, 0x7, 0x0, 0x30303, false }, /* PUPD15 */
    { 0x1cb0, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD16 */
    { 0x1cb8, PFC_PUPD, 4, 0x7, 0x0, 0x30303, false }, /* PUPD17 */
    { 0x1cc0, PFC_PUPD, 4, 0x7, 0x0, 0x30303, false }, /* PUPD18 */
    { 0x1cc8, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD19 */
    { 0x1cd0, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD1A */
    { 0x1cd8, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD1B */
    { 0x1ce0, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD1C */
    { 0x1ce8, PFC_PUPD, 4, 0x7, 0x0, 0x30303, false }, /* PUPD1D */
    { 0x1cf0, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD1E */
    { 0x1cf8, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD1F */
    { 0x1d00, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD20 */
    { 0x1d08, PFC_PUPD, 4, 0x7, 0x0, 0x30303, false }, /* PUPD21 */
    { 0x1d10, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD22 */
    { 0x1d18, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD23 */
    { 0x1db0, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD36 */
    { 0x1db8, PFC_PUPD, 4, 0x7, 0x0, 0x30303, false }, /* PUPD37 */
    { 0x1dc0, PFC_PUPD, 4, 0x7, 0x0, 0x30303, false }, /* PUPD38 */
    { 0x1dc8, PFC_PUPD, 4, 0x7, 0x0, 0x303, false }, /* PUPD39 */
    { 0x1dd0, PFC_PUPD, 4, 0x7, 0x0, 0x3030303, false }, /* PUPD3A_L */
    { 0x1dd4, PFC_PUPD, 4, 0x7, 0x0, 0x3, false }, /* PUPD3A_H */
    { 0x1dd8, PFC_PUPD, 4, 0x7, 0x0, 0x3030303, false }, /* PUPD3B */
    { 0x1de0, PFC_PUPD, 4, 0x7, 0x0, 0x3030303, false }, /* PUPD3C */
    { 0x1de8, PFC_PUPD, 4, 0x7, 0x0, 0x3030303, false }, /* PUPD3D */
    { 0x1df0, PFC_PUPD, 4, 0x7, 0x0, 0x3030303, false }, /* PUPD3E */
    { 0x1df8, PFC_PUPD, 4, 0x7, 0x0, 0x3030303, false }, /* PUPD3F */
    { 0x1e00, PFC_PUPD, 4, 0x7, 0x0, 0x3030303, false }, /* PUPD40_L */
    { 0x1e04, PFC_PUPD, 4, 0x7, 0x0, 0x3, false }, /* PUPD40_H */
    { 0x2008, PFC_FILON, 4, 0x7, 0x0, 0x1, false }, /* FILONOFF01 */
    { 0x2080, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF10 */
    { 0x2088, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF11 */
    { 0x2090, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF12 */
    { 0x2098, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF13 */
    { 0x20a0, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF14 */
    { 0x20a8, PFC_FILON, 4, 0x7, 0x0, 0x10101, false }, /* FILONOFF15 */
    { 0x20b0, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF16 */
    { 0x20b8, PFC_FILON, 4, 0x7, 0x0, 0x10101, false }, /* FILONOFF17 */
    { 0x20c0, PFC_FILON, 4, 0x7, 0x0, 0x10101, false }, /* FILONOFF18 */
    { 0x20c8, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF19 */
    { 0x20d0, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF1A */
    { 0x20d8, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF1B */
    { 0x20e0, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF1C */
    { 0x20e8, PFC_FILON, 4, 0x7, 0x0, 0x10101, false }, /* FILONOFF1D */
    { 0x20f0, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF1E */
    { 0x20f8, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF1F */
    { 0x2100, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF20 */
    { 0x2108, PFC_FILON, 4, 0x7, 0x0, 0x10101, false }, /* FILONOFF21 */
    { 0x2110, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF22 */
    { 0x2118, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF23 */
    { 0x2120, PFC_FILON, 4, 0x7, 0x0, 0x10101, false }, /* FILONOFF24 */
    { 0x2128, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF25 */
    { 0x2130, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF26 */
    { 0x2138, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF27 */
    { 0x2140, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF28 */
    { 0x2148, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF29 */
    { 0x2150, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF2A */
    { 0x2158, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF2B */
    { 0x2160, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF2C */
    { 0x2168, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF2D */
    { 0x2170, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF2E */
    { 0x2178, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF2F */
    { 0x2180, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF30 */
    { 0x2188, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF31 */
    { 0x2190, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF32 */
    { 0x2198, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF33 */
    { 0x21a0, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF34 */
    { 0x21a8, PFC_FILON, 4, 0x7, 0x0, 0x10101, false }, /* FILONOFF35 */
    { 0x21b0, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF36 */
    { 0x21b8, PFC_FILON, 4, 0x7, 0x0, 0x10101, false }, /* FILONOFF37 */
    { 0x21c0, PFC_FILON, 4, 0x7, 0x0, 0x10101, false }, /* FILONOFF38 */
    { 0x21c8, PFC_FILON, 4, 0x7, 0x0, 0x101, false }, /* FILONOFF39 */
    { 0x21d0, PFC_FILON, 4, 0x7, 0x0, 0x1010101, false }, /* FILONOFF3A_L */
    { 0x21d4, PFC_FILON, 4, 0x7, 0x0, 0x1, false }, /* FILONOFF3A_H */
    { 0x21d8, PFC_FILON, 4, 0x7, 0x0, 0x1010101, false }, /* FILONOFF3B */
    { 0x21e0, PFC_FILON, 4, 0x7, 0x0, 0x1010101, false }, /* FILONOFF3C */
    { 0x21e8, PFC_FILON, 4, 0x7, 0x0, 0x1010101, false }, /* FILONOFF3D */
    { 0x21f0, PFC_FILON, 4, 0x7, 0x0, 0x1010101, false }, /* FILONOFF3E */
    { 0x21f8, PFC_FILON, 4, 0x7, 0x0, 0x1010101, false }, /* FILONOFF3F */
    { 0x2200, PFC_FILON, 4, 0x7, 0x0, 0x1010101, false }, /* FILONOFF40_L */
    { 0x2204, PFC_FILON, 4, 0x7, 0x0, 0x1, false }, /* FILONOFF40_H */
    { 0x2408, PFC_FILNUM, 4, 0x7, 0x0, 0x3, false }, /* FILNUM01 */
    { 0x2480, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM10 */
    { 0x2488, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM11 */
    { 0x2490, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM12 */
    { 0x2498, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM13 */
    { 0x24a0, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM14 */
    { 0x24a8, PFC_FILNUM, 4, 0x7, 0x0, 0x30303, false }, /* FILNUM15 */
    { 0x24b0, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM16 */
    { 0x24b8, PFC_FILNUM, 4, 0x7, 0x0, 0x30303, false }, /* FILNUM17 */
    { 0x24c0, PFC_FILNUM, 4, 0x7, 0x0, 0x30303, false }, /* FILNUM18 */
    { 0x24c8, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM19 */
    { 0x24d0, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM1A */
    { 0x24d8, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM1B */
    { 0x24e0, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM1C */
    { 0x24e8, PFC_FILNUM, 4, 0x7, 0x0, 0x30303, false }, /* FILNUM1D */
    { 0x24f0, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM1E */
    { 0x24f8, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM1F */
    { 0x2500, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM20 */
    { 0x2508, PFC_FILNUM, 4, 0x7, 0x0, 0x30303, false }, /* FILNUM21 */
    { 0x2510, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM22 */
    { 0x2518, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM23 */
    { 0x2520, PFC_FILNUM, 4, 0x7, 0x0, 0x30303, false }, /* FILNUM24 */
    { 0x2528, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM25 */
    { 0x2530, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM26 */
    { 0x2538, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM27 */
    { 0x2540, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM28 */
    { 0x2548, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM29 */
    { 0x2550, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM2A */
    { 0x2558, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM2B */
    { 0x2560, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM2C */
    { 0x2568, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM2D */
    { 0x2570, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM2E */
    { 0x2578, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM2F */
    { 0x2580, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM30 */
    { 0x2588, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM31 */
    { 0x2590, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM32 */
    { 0x2598, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM33 */
    { 0x25a0, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM34 */
    { 0x25a8, PFC_FILNUM, 4, 0x7, 0x0, 0x30303, false }, /* FILNUM35 */
    { 0x25b0, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM36 */
    { 0x25b8, PFC_FILNUM, 4, 0x7, 0x0, 0x30303, false }, /* FILNUM37 */
    { 0x25c0, PFC_FILNUM, 4, 0x7, 0x0, 0x30303, false }, /* FILNUM38 */
    { 0x25c8, PFC_FILNUM, 4, 0x7, 0x0, 0x303, false }, /* FILNUM39 */
    { 0x25d0, PFC_FILNUM, 4, 0x7, 0x0, 0x3030303, false }, /* FILNUM3A_L */
    { 0x25d4, PFC_FILNUM, 4, 0x7, 0x0, 0x3, false }, /* FILNUM3A_H */
    { 0x25d8, PFC_FILNUM, 4, 0x7, 0x0, 0x3030303, false }, /* FILNUM3B */
    { 0x25e0, PFC_FILNUM, 4, 0x7, 0x0, 0x3030303, false }, /* FILNUM3C */
    { 0x25e8, PFC_FILNUM, 4, 0x7, 0x0, 0x3030303, false }, /* FILNUM3D */
    { 0x25f0, PFC_FILNUM, 4, 0x7, 0x0, 0x3030303, false }, /* FILNUM3E */
    { 0x25f8, PFC_FILNUM, 4, 0x7, 0x0, 0x3030303, false }, /* FILNUM3F */
    { 0x2600, PFC_FILNUM, 4, 0x7, 0x0, 0x3030303, false }, /* FILNUM40_L */
    { 0x2604, PFC_FILNUM, 4, 0x7, 0x0, 0x3, false }, /* FILNUM40_H */
    { 0x2808, PFC_FILCLK, 4, 0x7, 0x0, 0x3, false }, /* FILCLKSEL01 */
    { 0x2880, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL10 */
    { 0x2888, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL11 */
    { 0x2890, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL12 */
    { 0x2898, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL13 */
    { 0x28a0, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL14 */
    { 0x28a8, PFC_FILCLK, 4, 0x7, 0x0, 0x30303, false }, /* FILCLKSEL15 */
    { 0x28b0, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL16 */
    { 0x28b8, PFC_FILCLK, 4, 0x7, 0x0, 0x30303, false }, /* FILCLKSEL17 */
    { 0x28c0, PFC_FILCLK, 4, 0x7, 0x0, 0x30303, false }, /* FILCLKSEL18 */
    { 0x28c8, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL19 */
    { 0x28d0, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL1A */
    { 0x28d8, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL1B */
    { 0x28e0, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL1C */
    { 0x28e8, PFC_FILCLK, 4, 0x7, 0x0, 0x30303, false }, /* FILCLKSEL1D */
    { 0x28f0, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL1E */
    { 0x28f8, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL1F */
    { 0x2900, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL20 */
    { 0x2908, PFC_FILCLK, 4, 0x7, 0x0, 0x30303, false }, /* FILCLKSEL21 */
    { 0x2910, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL22 */
    { 0x2918, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL23 */
    { 0x2920, PFC_FILCLK, 4, 0x7, 0x0, 0x30303, false }, /* FILCLKSEL24 */
    { 0x2928, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL25 */
    { 0x2930, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL26 */
    { 0x2938, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL27 */
    { 0x2940, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL28 */
    { 0x2948, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL29 */
    { 0x2950, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL2A */
    { 0x2958, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL2B */
    { 0x2960, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL2C */
    { 0x2968, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL2D */
    { 0x2970, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL2E */
    { 0x2978, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL2F */
    { 0x2980, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL30 */
    { 0x2988, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL31 */
    { 0x2990, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL32 */
    { 0x2998, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL33 */
    { 0x29a0, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL34 */
    { 0x29a8, PFC_FILCLK, 4, 0x7, 0x0, 0x30303, false }, /* FILCLKSEL35 */
    { 0x29b0, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL36 */
    { 0x29b8, PFC_FILCLK, 4, 0x7, 0x0, 0x30303, false }, /* FILCLKSEL37 */
    { 0x29c0, PFC_FILCLK, 4, 0x7, 0x0, 0x30303, false }, /* FILCLKSEL38 */
    { 0x29c8, PFC_FILCLK, 4, 0x7, 0x0, 0x303, false }, /* FILCLKSEL39 */
    { 0x29d0, PFC_FILCLK, 4, 0x7, 0x0, 0x3030303, false }, /* FILCLKSEL3A_L */
    { 0x29d4, PFC_FILCLK, 4, 0x7, 0x0, 0x3, false }, /* FILCLKSEL3A_H */
    { 0x29d8, PFC_FILCLK, 4, 0x7, 0x0, 0x3030303, false }, /* FILCLKSEL3B */
    { 0x29e0, PFC_FILCLK, 4, 0x7, 0x0, 0x3030303, false }, /* FILCLKSEL3C */
    { 0x29e8, PFC_FILCLK, 4, 0x7, 0x0, 0x3030303, false }, /* FILCLKSEL3D */
    { 0x29f0, PFC_FILCLK, 4, 0x7, 0x0, 0x3030303, false }, /* FILCLKSEL3E */
    { 0x29f8, PFC_FILCLK, 4, 0x7, 0x0, 0x3030303, false }, /* FILCLKSEL3F */
    { 0x2a00, PFC_FILCLK, 4, 0x7, 0x0, 0x3030303, false }, /* FILCLKSEL40_L */
    { 0x2a04, PFC_FILCLK, 4, 0x7, 0x0, 0x3, false }, /* FILCLKSEL40_H */
    { 0x2c80, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL10 */
    { 0x2c88, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL11 */
    { 0x2c90, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL12 */
    { 0x2c98, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL13 */
    { 0x2ca0, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL14 */
    { 0x2ca8, PFC_ISEL, 4, 0x7, 0x0, 0x10101, false }, /* ISEL15 */
    { 0x2cb0, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL16 */
    { 0x2cb8, PFC_ISEL, 4, 0x7, 0x0, 0x10101, false }, /* ISEL17 */
    { 0x2cc0, PFC_ISEL, 4, 0x7, 0x0, 0x10101, false }, /* ISEL18 */
    { 0x2cc8, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL19 */
    { 0x2cd0, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL1A */
    { 0x2cd8, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL1B */
    { 0x2ce0, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL1C */
    { 0x2ce8, PFC_ISEL, 4, 0x7, 0x0, 0x10101, false }, /* ISEL1D */
    { 0x2cf0, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL1E */
    { 0x2cf8, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL1F */
    { 0x2d00, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL20 */
    { 0x2d08, PFC_ISEL, 4, 0x7, 0x0, 0x10101, false }, /* ISEL21 */
    { 0x2d10, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL22 */
    { 0x2d18, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL23 */
    { 0x2d20, PFC_ISEL, 4, 0x7, 0x0, 0x10101, false }, /* ISEL24 */
    { 0x2d28, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL25 */
    { 0x2d30, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL26 */
    { 0x2d38, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL27 */
    { 0x2d40, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL28 */
    { 0x2d48, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL29 */
    { 0x2d50, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL2A */
    { 0x2d58, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL2B */
    { 0x2d60, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL2C */
    { 0x2d68, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL2D */
    { 0x2d70, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL2E */
    { 0x2d78, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL2F */
    { 0x2d80, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL30 */
    { 0x2d88, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL31 */
    { 0x2d90, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL32 */
    { 0x2d98, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL33 */
    { 0x2da0, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL34 */
    { 0x2da8, PFC_ISEL, 4, 0x7, 0x0, 0x10101, false }, /* ISEL35 */
    { 0x2db0, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL36 */
    { 0x2db8, PFC_ISEL, 4, 0x7, 0x0, 0x10101, false }, /* ISEL37 */
    { 0x2dc0, PFC_ISEL, 4, 0x7, 0x0, 0x10101, false }, /* ISEL38 */
    { 0x2dc8, PFC_ISEL, 4, 0x7, 0x0, 0x101, false }, /* ISEL39 */
    { 0x2dd0, PFC_ISEL, 4, 0x7, 0x0, 0x1010101, false }, /* ISEL3A_L */
    { 0x2dd4, PFC_ISEL, 4, 0x7, 0x0, 0x1, false }, /* ISEL3A_H */
    { 0x2dd8, PFC_ISEL, 4, 0x7, 0x0, 0x1010101, false }, /* ISEL3B */
    { 0x2de0, PFC_ISEL, 4, 0x7, 0x0, 0x1010101, false }, /* ISEL3C */
    { 0x2de8, PFC_ISEL, 4, 0x7, 0x0, 0x1010101, false }, /* ISEL3D */
    { 0x2df0, PFC_ISEL, 4, 0x7, 0x0, 0x1010101, false }, /* ISEL3E */
    { 0x2df8, PFC_ISEL, 4, 0x7, 0x0, 0x1010101, false }, /* ISEL3F */
    { 0x2e00, PFC_ISEL, 4, 0x7, 0x0, 0x1010101, false }, /* ISEL40_L */
    { 0x2e04, PFC_ISEL, 4, 0x7, 0x0, 0x1, false }, /* ISEL40_H */
    { 0x3000, PFC_SD_CH0, 1, 0x1, 0x0, 0x1, false }, /* SD_ch0 */
    { 0x3004, PFC_SD_CH1, 1, 0x1, 0x0, 0x1, false }, /* SD_ch1 */
    { 0x3008, PFC_QSPI, 1, 0x1, 0x0, 0x1, false }, /* QSPI */
    { 0x300c, PFC_ETH_CH0, 1, 0x1, 0x0, 0x3, false }, /* ETH_ch0 */
    { 0x3010, PFC_ETH_CH1, 1, 0x1, 0x0, 0x3, false }, /* ETH_ch1 */
    { 0x3014, PFC_PWPR, 4, 0x5, 0x80, 0xc0, false }, /* PWPR */
    { 0x3018, PFC_ETH_MODE, 1, 0x1, 0x3, 0x3, false }, /* ETH_MII_RGMII */
};

QEMU_BUILD_BUG_ON(ARRAY_SIZE(pfc_regs) != RENESAS_RZG2L_PFC_NUM_REGS);

/*
 * Values the boot firmware (TF-A pfc_setup) leaves that differ from the
 * reset values, read on the RZ/V2L SMARC EVK at the U-Boot prompt: the
 * SCIF0 (P38), SD0 (P47) and SD1 (P19) pin functions, SD0 at 1.8 V, the
 * Ethernet TXC pins as outputs.
 */
static const struct {
    uint16_t offset;
    uint32_t value;
} pfc_post_firmware[] = {
    { 0x0223, 0x03 },           /* PMC23 (P19) */
    { 0x0236, 0x03 },           /* PMC36 (P38) */
    { 0x023f, 0x03 },           /* PMC3F (P47) */
    { 0x048c, 0x00000011 },     /* PFC23 */
    { 0x04d8, 0x00000011 },     /* PFC36 */
    { 0x04fc, 0x00000011 },     /* PFC3F */
    { 0x3000, 0x01 },           /* SD_ch0 */
    { 0x3018, 0x00 },           /* ETH MII/RGMII */
};

/* Register index per byte of the register space, or -1 */
static int16_t pfc_byte[PFC_REG_SPACE];

static int pfc_index(hwaddr offset)
{
    return offset < PFC_REG_SPACE ? pfc_byte[offset] : -1;
}

/* PINn: Pn for pins with PMn = 11b, 0 for the others */
static uint32_t pfc_pin_value(RenesasRZG2LPFCState *s, const PFCReg *r)
{
    unsigned off = r->offset - PIN_BASE;
    int p = pfc_index(P_BASE + off), pm = pfc_index(PM_BASE + 2 * off);
    uint32_t v = 0;
    int pin;

    for (pin = 0; pin < 8; pin++) {
        if (((s->regs[pm] >> (2 * pin)) & 3) == 3) {
            v |= s->regs[p] & (1u << pin);
        }
    }
    return v & r->mask;
}

static uint32_t pfc_reg_value(RenesasRZG2LPFCState *s, int i)
{
    if (pfc_regs[i].family == PFC_PIN) {
        return pfc_pin_value(s, &pfc_regs[i]);
    }
    return s->regs[i];
}

static void pfc_write_reg(RenesasRZG2LPFCState *s, int i, uint32_t value,
                          uint32_t bytes)
{
    const PFCReg *r = &pfc_regs[i];
    uint32_t m = r->mask & bytes;

    if (r->ro) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_rzg2l_pfc: write to "
                      "read-only register at 0x%x\n", r->offset);
        return;
    }
    if (r->family == PFC_PFC && !(s->regs[pfc_index(A_PWPR)] & PWPR_PFCWE)) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_rzg2l_pfc: PFC write at "
                      "0x%x while PWPR.PFCWE is 0 is dropped\n", r->offset);
        return;
    }
    if (r->family == PFC_PWPR) {
        uint32_t old = s->regs[i];

        if (old & PWPR_B0WI) {
            m &= ~PWPR_PFCWE;
        }
    }
    s->regs[i] = (s->regs[i] & ~m) | (value & m);
}

static void pfc_check_size(hwaddr offset, unsigned size, int i)
{
    const PFCReg *r = &pfc_regs[i];

    if (size > r->width || !(r->sizes & size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_rzg2l_pfc: %u-byte access "
                      "at 0x%" HWADDR_PRIx " to the %u-byte register at "
                      "0x%x\n", size, offset, r->width, r->offset);
    }
}

static uint64_t pfc_read(void *opaque, hwaddr offset, unsigned size)
{
    RenesasRZG2LPFCState *s = RENESAS_RZG2L_PFC(opaque);
    uint64_t value = 0;
    bool undefined = false;
    int last = -1;
    unsigned b;

    for (b = 0; b < size; b++) {
        int i = pfc_index(offset + b);

        if (i < 0) {
            undefined = true;
            continue;
        }
        if (i != last) {
            pfc_check_size(offset, size, i);
            last = i;
        }
        value |= (uint64_t)((pfc_reg_value(s, i) >>
                             (8 * (offset + b - pfc_regs[i].offset))) & 0xff)
                 << (8 * b);
    }
    if (undefined) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_rzg2l_pfc: %u-byte read at "
                      "0x%" HWADDR_PRIx " covers undefined bytes\n",
                      size, offset);
    }
    trace_renesas_rzg2l_pfc_read(offset, value, size);
    return value;
}

static void pfc_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    RenesasRZG2LPFCState *s = RENESAS_RZG2L_PFC(opaque);
    bool undefined = false;
    unsigned b = 0;

    trace_renesas_rzg2l_pfc_write(offset, value, size);
    while (b < size) {
        int i = pfc_index(offset + b);
        uint32_t data = 0, bytes = 0;
        unsigned shift;

        if (i < 0) {
            undefined = true;
            b++;
            continue;
        }
        pfc_check_size(offset, size, i);
        /* Gather the bytes of this access that fall in register i */
        for (; b < size && pfc_index(offset + b) == i; b++) {
            shift = 8 * (offset + b - pfc_regs[i].offset);
            data |= ((value >> (8 * b)) & 0xff) << shift;
            bytes |= 0xffu << shift;
        }
        pfc_write_reg(s, i, data, bytes);
    }
    if (undefined) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_rzg2l_pfc: %u-byte write at "
                      "0x%" HWADDR_PRIx " covers undefined bytes\n",
                      size, offset);
    }
}

static const MemoryRegionOps pfc_ops = {
    .read = pfc_read,
    .write = pfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static void pfc_reset_hold(Object *obj, ResetType type)
{
    RenesasRZG2LPFCState *s = RENESAS_RZG2L_PFC(obj);
    int i;

    for (i = 0; i < ARRAY_SIZE(pfc_regs); i++) {
        s->regs[i] = pfc_regs[i].reset;
    }
    if (s->direct_boot) {
        for (i = 0; i < ARRAY_SIZE(pfc_post_firmware); i++) {
            int r = pfc_index(pfc_post_firmware[i].offset);

            assert(r >= 0 && pfc_regs[r].offset == pfc_post_firmware[i].offset);
            s->regs[r] = pfc_post_firmware[i].value;
        }
    }
}

static void pfc_init(Object *obj)
{
    RenesasRZG2LPFCState *s = RENESAS_RZG2L_PFC(obj);

    memory_region_init_io(&s->iomem, obj, &pfc_ops, s, TYPE_RENESAS_RZG2L_PFC,
                          RENESAS_RZG2L_PFC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_pfc = {
    .name = TYPE_RENESAS_RZG2L_PFC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RenesasRZG2LPFCState,
                             RENESAS_RZG2L_PFC_NUM_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property pfc_props[] = {
    DEFINE_PROP_BOOL("direct-boot", RenesasRZG2LPFCState, direct_boot, false),
};

static void pfc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    int i, b;

    memset(pfc_byte, -1, sizeof(pfc_byte));
    for (i = 0; i < ARRAY_SIZE(pfc_regs); i++) {
        for (b = 0; b < pfc_regs[i].width; b++) {
            assert(pfc_regs[i].offset + b < PFC_REG_SPACE &&
                   pfc_byte[pfc_regs[i].offset + b] < 0);
            pfc_byte[pfc_regs[i].offset + b] = i;
        }
    }
    dc->vmsd = &vmstate_pfc;
    rc->phases.hold = pfc_reset_hold;
    device_class_set_props(dc, pfc_props);
}

static const TypeInfo pfc_info = {
    .name = TYPE_RENESAS_RZG2L_PFC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZG2LPFCState),
    .instance_init = pfc_init,
    .class_init = pfc_class_init,
};

static void pfc_register_types(void)
{
    type_register_static(&pfc_info);
}

type_init(pfc_register_types);
