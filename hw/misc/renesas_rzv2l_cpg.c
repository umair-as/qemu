/*
 * Renesas RZ/V2L Clock Pulse Generator (CPG)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The clock, reset and module-stop registers of manual Table 7.5, plus
 * the PLL1 setting registers, the dividers, the selectors and the two
 * other-function registers (OTHERFUNC1 bit 0 selects the PLL5_4 source;
 * OTHERFUNC2 bit 0 drives DDR_RST_N, which the model stores only).
 * Reset values, writable bits and write-enable bits come from the
 * register descriptions in section 7.2.4.
 *
 * - A field is written only when its write-enable bit (16 above the
 *   field's lowest bit) is set in the same write; the enable bits read
 *   as 0. Reserved bits keep their reset value.
 * - CPG_CLKMON_x and CPG_RSTMON_x (0x180 above CPG_CLKON_x and
 *   CPG_RST_x) are computed on read. A clock monitor bit is 1 while
 *   the clock is supplied, as in CPG_CLKON_x. A reset monitor bit is 1
 *   while the reset is applied, the inverse of CPG_RST_x. The model has
 *   no settle time, so a monitor follows its control register at once.
 *   The reserved bits 7:6 of CPG_CLKMON_WDT read as 0, as silicon does,
 *   where the manual gives them as 1.
 * - CPG_CLKSTATUS reads 0: every divider and selector switch is done
 *   when the write completes.
 *
 * - CPG_RST_SDHI drives the reset (IXRST) of each SD/MMC host interface
 *   channel (named GPIO output "sdhi-rst", high while the reset is
 *   applied).
 * - The watchdog: CPG_RST_WDT drives the PRESETN line of each WDT
 *   channel (named GPIO output "wdt-presetn", high while the reset is
 *   applied). A reset request from a channel (named GPIO input
 *   "wdtrstb", a level) resets the system while its CPG_WDTRST_SEL
 *   system-reset bit (2:0) is set, also when the bit is set after the
 *   request: CPG_WDTOVF_RST then records the channel. The flag is read
 *   after the release from the reset it caused (section 7.2.4.182), so
 *   no system reset clears it. A system reset initialises
 *   CPG_WDTRST_SEL: an inference from the note to section 7.2.4.183,
 *   which has a system reset return WDTRSTSEL7 to its initial value.
 *   Only power-on clears the flag (the model's reading). The cold
 *   reset and WDTOVFOUT pin selections (bits 10:8, 6:4) are stored
 *   only.
 * - The Cortex-A55 cores (sections 2.2, 7.2.4.186 to 7.2.4.189): each
 *   core's P-Channel accepts every power mode request at once
 *   (CPG_COREn_PCHMON.PACCEPT follows CPG_COREn_PCHCTL.PREQ, PDENY reads
 *   0) and takes the requested mode, ON or OFF. A core runs while its
 *   P-Channel is ON and CPG_RST_CA55 releases both its reset terminals,
 *   CA55_RST1_n and CA55_RST3_n (bits n and n + 2); named GPIO output
 *   "ca55-run" n changes when that does. After a power-on reset core 0
 *   runs and core 1 is off until software starts it (section 2.2.1).
 * Nothing else in the SoC sees the clock, reset or module-stop state.
 * The PLL monitors, PLL4, PLL6, PLL5 output settings and the Cortex-A55
 * cluster P-Channel are not modelled, nor are offsets the manual does
 * not list. Accesses to them fail with a decode error.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/watchdog.h"
#include "hw/core/irq.h"
#include "hw/misc/renesas_rzv2l_cpg.h"

#define CPG_REG_SPACE   0x1000
#define CPG_MON_OFFSET  0x180

typedef enum {
    CPG_PLAIN,      /* read/write, no write enables */
    CPG_WEN,        /* fields written under their write-enable bits */
    CPG_ZERO,       /* read-only, reads 0 */
    CPG_CLKMON,     /* computed from the CPG_CLKON_x 0x180 below */
    CPG_RSTMON,     /* computed from the CPG_RST_x 0x180 below */
    CPG_WDTOVF,     /* CPG_WDTOVF_RST: kept across system resets */
    CPG_PCHMON,     /* computed from the CPG_COREn_PCHCTL 4 above */
} CPGRegKind;

typedef struct {
    uint16_t offset;
    uint8_t kind;
    const char *name;
    uint32_t reset;
    uint32_t rw;        /* writable bits */
    uint16_t wen;       /* bit n set: bit (n + 16) enables the field at n */
} CPGReg;

#define CPG_RW(off, nm, rst, mask) \
    { .offset = off, .kind = CPG_PLAIN, .name = #nm, .reset = rst, .rw = mask }
#define CPG_WE(off, nm, rst, mask, en) \
    { .offset = off, .kind = CPG_WEN, .name = #nm, .reset = rst, \
      .rw = mask, .wen = en }
#define CPG_RO0(off, nm) \
    { .offset = off, .kind = CPG_ZERO, .name = #nm }
/* For a monitor, .reset holds its reserved bits that read as 1 */
#define CPG_MON(off, nm, fixed) \
    { .offset = off, .name = #nm, .reset = fixed, \
      .kind = (off) < 0x800 ? CPG_CLKMON : CPG_RSTMON }

static const CPGReg cpg_regs[] = {
    CPG_RW(0x004, SAMPLL1_CLK1, 0x00001901, 0xffffffff),
    CPG_RW(0x008, SAMPLL1_CLK2, 0x00180601, 0x00ff3f07),
    CPG_WE(0x140, SIPLL5_STBY, 0x00000014, 0x0015, 0x0015),
    CPG_WE(0x200, PL1_DDIV, 0x00000000, 0x0003, 0x0001),
    CPG_WE(0x204, PL2_DDIV, 0x00000000, 0x3007, 0x1001),
    CPG_WE(0x208, PL3A_DDIV, 0x00000000, 0x0777, 0x0111),
    CPG_WE(0x20c, PL3B_DDIV, 0x00000000, 0x0007, 0x0001),
    CPG_WE(0x210, PL6_DDIV, 0x00000000, 0x0003, 0x0001),
    CPG_WE(0x214, PL3C_SDIV, 0x00000401, 0x1f00, 0x0100),
    CPG_WE(0x218, PL2SDHI_DSEL, 0x00000011, 0x0033, 0x0011),
    CPG_WE(0x21c, PL4_DSEL, 0x00000000, 0x0001, 0x0001),
    CPG_RO0(0x280, CLKSTATUS),
    CPG_WE(0x408, PL3_SSEL, 0x00000111, 0x0100, 0x0100),
    CPG_WE(0x414, PL6_SSEL, 0x00000101, 0x1000, 0x1000),
    CPG_WE(0x418, PL6_ETH_SSEL, 0x00000010, 0x0001, 0x0001),
    CPG_WE(0x420, PL5_SDIV, 0x00000000, 0x0f03, 0x0101),
    CPG_WE(0x500, CLKON_CA55, 0x0000003f, 0x003f, 0x003f),
    CPG_WE(0x504, CLKON_CM33, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x508, CLKON_SRAM_ACPU, 0x00000001, 0x0001, 0x0001),
    CPG_WE(0x50c, CLKON_SRAM_MCPU, 0x00000001, 0x0001, 0x0001),
    CPG_WE(0x514, CLKON_GIC600, 0x00000001, 0x0001, 0x0001),
    CPG_WE(0x518, CLKON_IA55, 0x00000003, 0x0003, 0x0003),
    CPG_WE(0x51c, CLKON_IM33, 0x00000003, 0x0003, 0x0003),
    CPG_WE(0x520, CLKON_MHU, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x524, CLKON_CST, 0x000007ff, 0x07ff, 0x07ff),
    CPG_WE(0x528, CLKON_SYC, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x52c, CLKON_DMAC_REG, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x534, CLKON_GTM, 0x00000001, 0x0007, 0x0007),
    CPG_WE(0x538, CLKON_MTU, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x53c, CLKON_POE3, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x540, CLKON_GPT, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x544, CLKON_POEG, 0x00000000, 0x000f, 0x000f),
    CPG_WE(0x548, CLKON_WDT, 0x000000c3, 0x003f, 0x003f),
    CPG_WE(0x54c, CLKON_DDR, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x550, CLKON_SPI_MULTI, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x554, CLKON_SDHI, 0x00000000, 0x00ff, 0x00ff),
    CPG_WE(0x558, CLKON_3DGE, 0x00000000, 0x0007, 0x0007),
    CPG_WE(0x55c, CLKON_ISU, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x560, CLKON_VCPL4, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x564, CLKON_CRU, 0x00000000, 0x000f, 0x000f),
    CPG_WE(0x568, CLKON_MIPI_DSI, 0x00000000, 0x003f, 0x003f),
    CPG_WE(0x56c, CLKON_LCDC, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x570, CLKON_SSI, 0x00000000, 0x00ff, 0x00ff),
    CPG_WE(0x578, CLKON_USB, 0x00000000, 0x000f, 0x000f),
    CPG_WE(0x57c, CLKON_ETH, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x580, CLKON_I2C, 0x00000000, 0x000f, 0x000f),
    CPG_WE(0x584, CLKON_SCIF, 0x00000000, 0x001f, 0x001f),
    CPG_WE(0x588, CLKON_SCI, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x58c, CLKON_IRDA, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x590, CLKON_RSPI, 0x00000000, 0x0007, 0x0007),
    CPG_WE(0x594, CLKON_CANFD, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x598, CLKON_GPIO, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x5a8, CLKON_ADC, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x5ac, CLKON_TSU, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x5b8, CLKON_MBUS, 0x000007ff, 0x0208, 0x0208),
    CPG_WE(0x5dc, CLKON_AXI_TZCDDR, 0x0000001f, 0x001f, 0x001f),
    CPG_WE(0x5e8, CLKON_DRP, 0x00000000, 0x001f, 0x001f),
    CPG_MON(0x680, CLKMON_CA55, 0x0),
    CPG_MON(0x684, CLKMON_CM33, 0x0),
    CPG_MON(0x688, CLKMON_SRAM_ACPU, 0x0),
    CPG_MON(0x68c, CLKMON_SRAM_MCPU, 0x0),
    CPG_MON(0x694, CLKMON_GIC600, 0x0),
    CPG_MON(0x698, CLKMON_IA55, 0x0),
    CPG_MON(0x69c, CLKMON_IM33, 0x0),
    CPG_MON(0x6a0, CLKMON_MHU, 0x0),
    CPG_MON(0x6a4, CLKMON_CST, 0x0),
    CPG_MON(0x6a8, CLKMON_SYC, 0x0),
    CPG_MON(0x6ac, CLKMON_DMAC_REG, 0x0),
    CPG_MON(0x6b4, CLKMON_GTM, 0x0),
    CPG_MON(0x6b8, CLKMON_MTU, 0x0),
    CPG_MON(0x6bc, CLKMON_POE3, 0x0),
    CPG_MON(0x6c0, CLKMON_GPT, 0x0),
    CPG_MON(0x6c4, CLKMON_POEG, 0x0),
    CPG_MON(0x6c8, CLKMON_WDT, 0x0),
    CPG_MON(0x6cc, CLKMON_DDR, 0x0),
    CPG_MON(0x6d0, CLKMON_SPI_MULTI, 0x0),
    CPG_MON(0x6d4, CLKMON_SDHI, 0x0),
    CPG_MON(0x6d8, CLKMON_3DGE, 0x0),
    CPG_MON(0x6dc, CLKMON_ISU, 0x0),
    CPG_MON(0x6e0, CLKMON_VCPL4, 0x0),
    CPG_MON(0x6e4, CLKMON_CRU, 0x0),
    CPG_MON(0x6e8, CLKMON_MIPI_DSI, 0x0),
    CPG_MON(0x6ec, CLKMON_LCDC, 0x0),
    CPG_MON(0x6f0, CLKMON_SSI, 0x0),
    CPG_MON(0x6f8, CLKMON_USB, 0x0),
    CPG_MON(0x6fc, CLKMON_ETH, 0x0),
    CPG_MON(0x700, CLKMON_I2C, 0x0),
    CPG_MON(0x704, CLKMON_SCIF, 0x0),
    CPG_MON(0x708, CLKMON_SCI, 0x0),
    CPG_MON(0x70c, CLKMON_IRDA, 0x0),
    CPG_MON(0x710, CLKMON_RSPI, 0x0),
    CPG_MON(0x714, CLKMON_CANFD, 0x0),
    CPG_MON(0x718, CLKMON_GPIO, 0x0),
    CPG_MON(0x728, CLKMON_ADC, 0x0),
    CPG_MON(0x72c, CLKMON_TSU, 0x0),
    CPG_MON(0x738, CLKMON_MBUS, 0x5f7),
    CPG_MON(0x75c, CLKMON_AXI_TZCDDR, 0x0),
    CPG_MON(0x768, CLKMON_DRP, 0x0),
    CPG_WE(0x800, RST_CA55, 0x00001fff, 0x1fff, 0x1fff),
    CPG_WE(0x804, RST_CM33, 0x00000000, 0x0007, 0x0007),
    CPG_WE(0x808, RST_SRAM_ACPU, 0x00000001, 0x0001, 0x0001),
    CPG_WE(0x80c, RST_SRAM_MCPU, 0x00000001, 0x0001, 0x0001),
    CPG_WE(0x814, RST_GIC600, 0x00000003, 0x0003, 0x0003),
    CPG_WE(0x818, RST_IA55, 0x00000001, 0x0001, 0x0001),
    CPG_WE(0x81c, RST_IM33, 0x00000001, 0x0001, 0x0001),
    CPG_WE(0x820, RST_MHU, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x828, RST_SYC, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x82c, RST_DMAC, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x834, RST_GTM, 0x00000001, 0x0007, 0x0007),
    CPG_WE(0x838, RST_MTU, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x83c, RST_POE3, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x840, RST_GPT, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x844, RST_POEG, 0x00000000, 0x000f, 0x000f),
    CPG_WE(0x848, RST_WDT, 0x00000009, 0x0007, 0x0007),
    CPG_WE(0x84c, RST_DDR, 0x00000000, 0x007f, 0x007f),
    CPG_WE(0x850, RST_SPI, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x854, RST_SDHI, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x858, RST_3DGE, 0x00000000, 0x0007, 0x0007),
    CPG_WE(0x85c, RST_ISU, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x860, RST_VCPL4, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x864, RST_CRU, 0x00000000, 0x0007, 0x0007),
    CPG_WE(0x868, RST_MIPI_DSI, 0x00000000, 0x0007, 0x0007),
    CPG_WE(0x86c, RST_LCDC, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x870, RST_SSIF, 0x00000000, 0x000f, 0x000f),
    CPG_WE(0x878, RST_USB, 0x00000000, 0x000f, 0x000f),
    CPG_WE(0x87c, RST_ETH, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x880, RST_I2C, 0x00000000, 0x000f, 0x000f),
    CPG_WE(0x884, RST_SCIF, 0x00000000, 0x001f, 0x001f),
    CPG_WE(0x888, RST_SCI, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x88c, RST_IRDA, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x890, RST_RSPI, 0x00000000, 0x0007, 0x0007),
    CPG_WE(0x894, RST_CANFD, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x898, RST_GPIO, 0x00000000, 0x0007, 0x0007),
    CPG_WE(0x8a8, RST_ADC, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0x8ac, RST_TSU, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0x8dc, RST_AXI_TZCDDR, 0x0000001f, 0x001f, 0x001f),
    CPG_WE(0x8e8, RST_DRP, 0x00000000, 0x0001, 0x0001),
    CPG_MON(0x980, RSTMON_CA55, 0),
    CPG_MON(0x984, RSTMON_CM33, 0),
    CPG_MON(0x988, RSTMON_SRAM_ACPU, 0),
    CPG_MON(0x98c, RSTMON_SRAM_MCPU, 0),
    CPG_MON(0x994, RSTMON_GIC600, 0),
    CPG_MON(0x998, RSTMON_IA55, 0),
    CPG_MON(0x99c, RSTMON_IM33, 0),
    CPG_MON(0x9a0, RSTMON_MHU, 0),
    CPG_MON(0x9a8, RSTMON_SYC, 0),
    CPG_MON(0x9ac, RSTMON_DMAC, 0),
    CPG_MON(0x9b4, RSTMON_GTM, 0),
    CPG_MON(0x9b8, RSTMON_MTU, 0),
    CPG_MON(0x9bc, RSTMON_POE3, 0),
    CPG_MON(0x9c0, RSTMON_GPT, 0),
    CPG_MON(0x9c4, RSTMON_POEG, 0),
    CPG_MON(0x9c8, RSTMON_WDT, 0),
    CPG_MON(0x9cc, RSTMON_DDR, 0),
    CPG_MON(0x9d0, RSTMON_SPI, 0),
    CPG_MON(0x9d4, RSTMON_SDHI, 0),
    CPG_MON(0x9d8, RSTMON_3DGE, 0),
    CPG_MON(0x9dc, RSTMON_ISU, 0),
    CPG_MON(0x9e0, RSTMON_VCPL4, 0),
    CPG_MON(0x9e4, RSTMON_CRU, 0),
    CPG_MON(0x9e8, RSTMON_MIPI_DSI, 0),
    CPG_MON(0x9ec, RSTMON_LCDC, 0),
    CPG_MON(0x9f0, RSTMON_SSIF, 0),
    CPG_MON(0x9f8, RSTMON_USB, 0),
    CPG_MON(0x9fc, RSTMON_ETH, 0),
    CPG_MON(0xa00, RSTMON_I2C, 0),
    CPG_MON(0xa04, RSTMON_SCIF, 0),
    CPG_MON(0xa08, RSTMON_SCI, 0),
    CPG_MON(0xa0c, RSTMON_IRDA, 0),
    CPG_MON(0xa10, RSTMON_RSPI, 0),
    CPG_MON(0xa14, RSTMON_CANFD, 0),
    CPG_MON(0xa18, RSTMON_GPIO, 0),
    CPG_MON(0xa28, RSTMON_ADC, 0),
    CPG_MON(0xa2c, RSTMON_TSU, 0),
    CPG_MON(0xa5c, RSTMON_AXI_TZCDDR, 0),
    CPG_MON(0xa68, RSTMON_DRP, 0),
    { .offset = 0xb10, .kind = CPG_WDTOVF, .name = "WDTOVF_RST" },
    CPG_RW(0xb14, WDTRST_SEL, 0x00000088, 0x00000777),
    { .offset = 0xb38, .kind = CPG_PCHMON, .name = "CORE0_PCHMON" },
    CPG_RW(0xb3c, CORE0_PCHCTL, 0x00080000, 0x003f0001),
    { .offset = 0xb40, .kind = CPG_PCHMON, .name = "CORE1_PCHMON" },
    CPG_RW(0xb44, CORE1_PCHCTL, 0x00080000, 0x003f0001),
    CPG_WE(0xb60, BUS_ACPU_MSTOP, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0xb64, BUS_MCPU1_MSTOP, 0x00000000, 0xfff7, 0xffff),
    CPG_WE(0xb68, BUS_MCPU2_MSTOP, 0x00000000, 0xffff, 0xffff),
    CPG_WE(0xb6c, BUS_PERI_COM_MSTOP, 0x00000000, 0x01ff, 0x01ff),
    CPG_WE(0xb70, BUS_PERI_CPU_MSTOP, 0x00000000, 0xffdf, 0xffdf),
    CPG_WE(0xb74, BUS_PERI_DDR_MSTOP, 0x00000000, 0x0003, 0x0003),
    CPG_WE(0xb78, BUS_PERI_VIDEO_MSTOP, 0x00000000, 0x07ff, 0x07ff),
    CPG_WE(0xb7c, BUS_REG0_MSTOP, 0x00000000, 0x007e, 0x007e),
    CPG_WE(0xb80, BUS_REG1_MSTOP, 0x00000000, 0x00ff, 0x00ff),
    CPG_WE(0xb84, BUS_TZCDDR_MSTOP, 0x00000000, 0x000f, 0x000f),
    CPG_WE(0xb88, MHU_MSTOP, 0x00000000, 0x0001, 0x0001),
    CPG_WE(0xbe8, OTHERFUNC1_REG, 0x00000000, 0xffff, 0xffff),
    CPG_WE(0xbec, OTHERFUNC2_REG, 0x00000000, 0xffff, 0xffff),
};

QEMU_BUILD_BUG_ON(ARRAY_SIZE(cpg_regs) != RENESAS_RZV2L_CPG_NUM_REGS);

/*
 * Values the boot firmware (TF-A, then U-Boot) leaves in the registers
 * that differ from their reset value, read on the RZ/V2L SMARC EVK at
 * the U-Boot prompt. The monitors follow from these.
 */
static const struct {
    uint16_t offset;
    uint32_t value;
} cpg_post_firmware[] = {
    { 0x208, 0x00000100 },   /* PL3A_DDIV */
    { 0x218, 0x00000022 },   /* PL2SDHI_DSEL */
    { 0x21c, 0x00000001 },   /* PL4_DSEL */
    { 0x408, 0x00000011 },   /* PL3_SSEL */
    { 0x528, 0x00000001 },   /* CLKON_SYC */
    { 0x534, 0x00000000 },   /* CLKON_GTM */
    { 0x548, 0x000000ff },   /* CLKON_WDT */
    { 0x54c, 0x00000003 },   /* CLKON_DDR */
    { 0x550, 0x00000003 },   /* CLKON_SPI_MULTI */
    { 0x554, 0x000000ff },   /* CLKON_SDHI */
    { 0x578, 0x0000000f },   /* CLKON_USB */
    { 0x584, 0x00000001 },   /* CLKON_SCIF */
    { 0x598, 0x00000001 },   /* CLKON_GPIO */
    { 0x828, 0x00000001 },   /* RST_SYC */
    { 0x848, 0x0000000f },   /* RST_WDT */
    { 0x84c, 0x0000007f },   /* RST_DDR */
    { 0x850, 0x00000001 },   /* RST_SPI */
    { 0x854, 0x00000003 },   /* RST_SDHI */
    { 0x878, 0x0000000f },   /* RST_USB */
    { 0x87c, 0x00000003 },   /* RST_ETH */
    { 0x880, 0x0000000f },   /* RST_I2C */
    { 0x884, 0x00000001 },   /* RST_SCIF */
    { 0x898, 0x00000007 },   /* RST_GPIO */
    /* Not read on the EVK: TF-A's cpg_setup sets the system-reset bits */
    { 0xb14, 0x0000008f },   /* WDTRST_SEL */
};

/* Register table index by offset / 4, or -1 */
static int16_t cpg_index[CPG_REG_SPACE / 4];

static const CPGReg *cpg_lookup(hwaddr offset, int *idx)
{
    if (offset >= CPG_REG_SPACE || (offset & 3) ||
        cpg_index[offset / 4] < 0) {
        return NULL;
    }
    *idx = cpg_index[offset / 4];
    return &cpg_regs[*idx];
}

#define CPG_RST_CA55        0x800
#define CPG_RST_WDT         0x848
#define CPG_RST_SDHI        0x854
#define CPG_WDTRST_SEL      0xb14
#define CPG_WDTOVF_MASK     0x7
#define CPG_CORE0_PCHCTL    0xb3c
#define CPG_CORE1_PCHCTL    0xb44
#define CPG_PCHCTL_PREQ     BIT(0)
#define CPG_PCHCTL_PSTATE(v)    extract32(v, 16, 6)
#define CPG_PSTATE_ON       0x08
#define CPG_PSTATE_OFF_EMU  0x01
#define CPG_PSTATE_OFF      0x00

/*
 * Channel @n requests a reset: WDTRSTB is a level, so a request that was
 * masked acts once CPG_WDTRST_SEL unmasks it while it is still held.
 */
static void rzv2l_cpg_wdt_request(RenesasRZV2LCPGState *s, int n)
{
    if (!(s->regs[cpg_index[CPG_WDTRST_SEL / 4]] & BIT(n))) {
        trace_renesas_rzv2l_cpg_wdt_reset(n, false);
        return;
    }
    trace_renesas_rzv2l_cpg_wdt_reset(n, true);
    s->wdtovf |= BIT(n);
    watchdog_perform_action();
}

/* PRESETN of each watchdog channel: held while its RST_WDT bit is 0 */
static void rzv2l_cpg_update_wdt(RenesasRZV2LCPGState *s)
{
    uint32_t rst = s->regs[cpg_index[CPG_RST_WDT / 4]];
    int n;

    for (n = 0; n < RENESAS_RZV2L_CPG_NUM_WDT; n++) {
        qemu_set_irq(s->wdt_presetn[n], !(rst & BIT(n)));
    }
}

/* IXRST of each SD/MMC host interface channel: held while its bit is 0 */
static void rzv2l_cpg_update_sdhi(RenesasRZV2LCPGState *s)
{
    uint32_t rst = s->regs[cpg_index[CPG_RST_SDHI / 4]];
    int n;

    for (n = 0; n < RENESAS_RZV2L_CPG_NUM_SDHI; n++) {
        qemu_set_irq(s->sdhi_rst[n], !(rst & BIT(n)));
    }
}

/* A Cortex-A55 core runs while its P-Channel is ON and its resets released */
static void rzv2l_cpg_update_ca55(RenesasRZV2LCPGState *s)
{
    uint32_t rst = s->regs[cpg_index[CPG_RST_CA55 / 4]];
    bool run;
    int n;

    for (n = 0; n < RENESAS_RZV2L_CPG_NUM_CA55; n++) {
        run = s->ca55_on[n] && (rst & BIT(n)) && (rst & BIT(n + 2));
        if (run != s->ca55_run[n]) {
            s->ca55_run[n] = run;
            trace_renesas_rzv2l_cpg_ca55_run(n, run);
            qemu_set_irq(s->ca55_run_out[n], run);
        }
    }
}

/* A P-Channel request (PREQ rising) is accepted and takes its mode */
static void rzv2l_cpg_pchannel(RenesasRZV2LCPGState *s, int n, uint32_t ctl)
{
    switch (CPG_PCHCTL_PSTATE(ctl)) {
    case CPG_PSTATE_ON:
        s->ca55_on[n] = true;
        break;
    case CPG_PSTATE_OFF:
    case CPG_PSTATE_OFF_EMU:
        s->ca55_on[n] = false;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: core %d: unsupported power mode 0x%x\n",
                      __func__, n, CPG_PCHCTL_PSTATE(ctl));
        return;
    }
    rzv2l_cpg_update_ca55(s);
}

/* The bits a write changes: each enabled field, as wide as it is */
static uint32_t cpg_wen_mask(const CPGReg *r, uint32_t value)
{
    uint32_t enabled = (value >> 16) & r->wen;
    uint32_t mask = 0;
    int bit;

    while (enabled) {
        bit = ctz32(enabled);
        enabled &= enabled - 1;
        do {
            mask |= 1u << bit;
            bit++;
        } while (bit < 16 && (r->rw & (1u << bit)) && !(r->wen & (1u << bit)));
    }
    return mask & r->rw;
}

static uint32_t cpg_monitor(RenesasRZV2LCPGState *s, const CPGReg *r)
{
    int idx = cpg_index[(r->offset - CPG_MON_OFFSET) / 4];
    const CPGReg *ctl = &cpg_regs[idx];

    if (r->kind == CPG_CLKMON) {
        return (s->regs[idx] & ctl->rw) | r->reset;
    }
    return ~s->regs[idx] & ctl->rw;
}

static MemTxResult rzv2l_cpg_read(void *opaque, hwaddr offset,
                                  uint64_t *data, unsigned size,
                                  MemTxAttrs attrs)
{
    RenesasRZV2LCPGState *s = RENESAS_RZV2L_CPG(opaque);
    const CPGReg *r;
    uint32_t value;
    int idx;

    r = cpg_lookup(offset, &idx);
    if (!r) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }

    switch (r->kind) {
    case CPG_CLKMON:
    case CPG_RSTMON:
        value = cpg_monitor(s, r);
        break;
    case CPG_ZERO:
        value = 0;
        break;
    case CPG_WDTOVF:
        value = s->wdtovf;
        break;
    case CPG_PCHMON:
        /* PACCEPT follows PREQ; nothing is ever denied */
        value = s->regs[cpg_index[(offset + 4) / 4]] & CPG_PCHCTL_PREQ;
        break;
    default:
        value = s->regs[idx];
        break;
    }

    trace_renesas_rzv2l_cpg_read(r->name, offset, value);
    *data = value;
    return MEMTX_OK;
}

static MemTxResult rzv2l_cpg_write(void *opaque, hwaddr offset,
                                   uint64_t data, unsigned size,
                                   MemTxAttrs attrs)
{
    RenesasRZV2LCPGState *s = RENESAS_RZV2L_CPG(opaque);
    const CPGReg *r;
    uint32_t mask, old;
    int idx, n;

    r = cpg_lookup(offset, &idx);
    if (!r) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }

    trace_renesas_rzv2l_cpg_write(r->name, offset, data);

    switch (r->kind) {
    case CPG_PLAIN:
        mask = r->rw;
        break;
    case CPG_WEN:
        mask = cpg_wen_mask(r, data);
        break;
    case CPG_WDTOVF:
        /* A flag clears on 1, under its write-enable bit (16 above) */
        s->wdtovf &= ~(data & (data >> 16) & CPG_WDTOVF_MASK);
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only register %s\n",
                      __func__, r->name);
        return MEMTX_OK;
    }
    old = s->regs[idx];
    s->regs[idx] = (s->regs[idx] & ~mask) | (data & mask);
    if (offset == CPG_RST_CA55) {
        rzv2l_cpg_update_ca55(s);
    } else if ((offset == CPG_CORE0_PCHCTL || offset == CPG_CORE1_PCHCTL) &&
               (s->regs[idx] & ~old & CPG_PCHCTL_PREQ)) {
        rzv2l_cpg_pchannel(s, offset == CPG_CORE1_PCHCTL, s->regs[idx]);
    } else if (offset == CPG_RST_WDT) {
        rzv2l_cpg_update_wdt(s);
    } else if (offset == CPG_RST_SDHI) {
        rzv2l_cpg_update_sdhi(s);
    } else if (offset == CPG_WDTRST_SEL) {
        for (n = 0; n < RENESAS_RZV2L_CPG_NUM_WDT; n++) {
            if ((s->regs[idx] & ~old & BIT(n)) && s->wdtrstb[n]) {
                rzv2l_cpg_wdt_request(s, n);
            }
        }
    }
    return MEMTX_OK;
}

static const MemoryRegionOps rzv2l_cpg_ops = {
    .read_with_attrs = rzv2l_cpg_read,
    .write_with_attrs = rzv2l_cpg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rzv2l_cpg_reset_hold(Object *obj, ResetType type)
{
    RenesasRZV2LCPGState *s = RENESAS_RZV2L_CPG(obj);
    const CPGReg *r;
    int i, idx;

    for (i = 0; i < ARRAY_SIZE(cpg_regs); i++) {
        s->regs[i] = (cpg_regs[i].kind == CPG_PLAIN ||
                      cpg_regs[i].kind == CPG_WEN) ? cpg_regs[i].reset : 0;
    }
    /* Out of a power-on reset only core 0 runs; no change is signalled */
    for (i = 0; i < RENESAS_RZV2L_CPG_NUM_CA55; i++) {
        s->ca55_on[i] = s->ca55_run[i] = (i == 0);
    }
    if (s->direct_boot) {
        for (i = 0; i < ARRAY_SIZE(cpg_post_firmware); i++) {
            r = cpg_lookup(cpg_post_firmware[i].offset, &idx);
            assert(r && (r->kind == CPG_PLAIN || r->kind == CPG_WEN));
            s->regs[idx] = cpg_post_firmware[i].value;
        }
    }
}

static void rzv2l_cpg_reset_exit(Object *obj, ResetType type)
{
    rzv2l_cpg_update_wdt(RENESAS_RZV2L_CPG(obj));
    rzv2l_cpg_update_sdhi(RENESAS_RZV2L_CPG(obj));
}

/* The reset request of watchdog channel @n (section 7.2.4.183) */
static void rzv2l_cpg_wdtrstb(void *opaque, int n, int level)
{
    RenesasRZV2LCPGState *s = RENESAS_RZV2L_CPG(opaque);
    bool rising = level && !s->wdtrstb[n];

    s->wdtrstb[n] = level;
    if (rising) {
        rzv2l_cpg_wdt_request(s, n);
    }
}

static void rzv2l_cpg_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RenesasRZV2LCPGState *s = RENESAS_RZV2L_CPG(obj);

    memory_region_init_io(&s->iomem, obj, &rzv2l_cpg_ops, s,
                          TYPE_RENESAS_RZV2L_CPG,
                          RENESAS_RZV2L_CPG_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), s->wdt_presetn, "wdt-presetn",
                             RENESAS_RZV2L_CPG_NUM_WDT);
    qdev_init_gpio_out_named(DEVICE(obj), s->sdhi_rst, "sdhi-rst",
                             RENESAS_RZV2L_CPG_NUM_SDHI);
    qdev_init_gpio_in_named(DEVICE(obj), rzv2l_cpg_wdtrstb, "wdtrstb",
                            RENESAS_RZV2L_CPG_NUM_WDT);
    qdev_init_gpio_out_named(DEVICE(obj), s->ca55_run_out, "ca55-run",
                             RENESAS_RZV2L_CPG_NUM_CA55);
}

static const VMStateDescription vmstate_rzv2l_cpg = {
    .name = TYPE_RENESAS_RZV2L_CPG,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RenesasRZV2LCPGState,
                             RENESAS_RZV2L_CPG_NUM_REGS),
        VMSTATE_UINT32(wdtovf, RenesasRZV2LCPGState),
        VMSTATE_BOOL_ARRAY(wdtrstb, RenesasRZV2LCPGState,
                           RENESAS_RZV2L_CPG_NUM_WDT),
        VMSTATE_BOOL_ARRAY(ca55_on, RenesasRZV2LCPGState,
                           RENESAS_RZV2L_CPG_NUM_CA55),
        VMSTATE_BOOL_ARRAY(ca55_run, RenesasRZV2LCPGState,
                           RENESAS_RZV2L_CPG_NUM_CA55),
        VMSTATE_END_OF_LIST()
    }
};

static const Property rzv2l_cpg_props[] = {
    DEFINE_PROP_BOOL("direct-boot", RenesasRZV2LCPGState, direct_boot, false),
};

static void rzv2l_cpg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    int i;

    memset(cpg_index, -1, sizeof(cpg_index));
    for (i = 0; i < ARRAY_SIZE(cpg_regs); i++) {
        assert(cpg_regs[i].offset < CPG_REG_SPACE &&
               cpg_index[cpg_regs[i].offset / 4] < 0);
        cpg_index[cpg_regs[i].offset / 4] = i;
    }
    for (i = 0; i < ARRAY_SIZE(cpg_regs); i++) {
        if (cpg_regs[i].kind == CPG_CLKMON ||
            cpg_regs[i].kind == CPG_RSTMON) {
            int ctl = cpg_index[(cpg_regs[i].offset - CPG_MON_OFFSET) / 4];

            assert(ctl >= 0 && cpg_regs[ctl].kind == CPG_WEN);
        }
    }

    rc->phases.hold = rzv2l_cpg_reset_hold;
    rc->phases.exit = rzv2l_cpg_reset_exit;
    dc->vmsd = &vmstate_rzv2l_cpg;
    device_class_set_props(dc, rzv2l_cpg_props);
}

static const TypeInfo rzv2l_cpg_info = {
    .name = TYPE_RENESAS_RZV2L_CPG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZV2LCPGState),
    .instance_init = rzv2l_cpg_init,
    .class_init = rzv2l_cpg_class_init,
};

static void rzv2l_cpg_register_types(void)
{
    type_register_static(&rzv2l_cpg_info);
}

type_init(rzv2l_cpg_register_types);
