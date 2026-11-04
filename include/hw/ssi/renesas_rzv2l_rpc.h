/*
 * Renesas RZ/V2L SPI Multi I/O Bus Controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The SPI Multi I/O Bus Controller of the Renesas RZ/V2L, documented in
 * the RZ/V2L Group User's Manual: Hardware (R01UH0936EJ), section 25,
 * with one serial flash memory on QSPI0.
 *
 * QEMU interface:
 *  + sysbus MMIO region 0: the registers (64 KiB)
 *  + sysbus MMIO region 1: the SPI multi I/O bus space (256 MiB), read in
 *    external address space read mode
 *  + sysbus IRQ 0: SPIHF, the HyperFlash INT# input (never asserted: no
 *    HyperFlash is supported)
 *  + sysbus IRQ 1: QSPI0_SSL, the flash's chip select (low while
 *    asserted)
 *  + named GPIO input "rst": high while the clock controller holds the
 *    controller in reset
 *  + named GPIO input "clk": high while the clock controller supplies
 *    its clocks
 *  + child bus "spi": the flash
 */

#ifndef HW_SSI_RENESAS_RZV2L_RPC_H
#define HW_SSI_RENESAS_RZV2L_RPC_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_RENESAS_RZV2L_RPC "renesas-rzv2l-rpc"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRZV2LRPCState, RENESAS_RZV2L_RPC)

#define RENESAS_RZV2L_RPC_MMIO_SIZE     0x10000
#define RENESAS_RZV2L_RPC_WINDOW_SIZE   (256 * MiB)
#define RENESAS_RZV2L_RPC_CACHE_LINES   32
#define RENESAS_RZV2L_RPC_LINE_SIZE     8

struct RenesasRZV2LRPCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    MemoryRegion window;
    SSIBus *spi;
    qemu_irq irq;
    qemu_irq cs;

    /* Inputs from the clock controller */
    bool in_reset;
    bool clk_on;

    /* Registers (Table 25.2) */
    uint32_t cmncr;
    uint32_t ssldr;
    uint32_t drcr;
    uint32_t drcmr;
    uint32_t drear;
    uint32_t dropr;
    uint32_t drenr;
    uint32_t smcr;
    uint32_t smcmr;
    uint32_t smadr;
    uint32_t smopr;
    uint32_t smenr;
    uint32_t smrdr0;
    uint32_t smrdr1;
    uint32_t smwdr0;
    uint32_t smwdr1;
    uint32_t drdmcr;
    uint32_t drdrenr;
    uint32_t smdmcr;
    uint32_t smdrenr;
    uint32_t phyadj1;
    uint32_t phyadj2;
    uint32_t phycnt;
    uint32_t phyoffset1;
    uint32_t phyoffset2;
    uint32_t phyint;

    /* QSPI0_SSL asserted */
    bool ssl;
    /* Held after a burst read with SSLE = 1, up to next_addr */
    bool burst_held;
    uint32_t next_addr;
    bool ddr_logged;

    /* Read cache: 32 lines of 8 bytes, filled from the write pointer */
    uint32_t cache_wp;
    bool cache_valid[RENESAS_RZV2L_RPC_CACHE_LINES];
    uint32_t cache_tag[RENESAS_RZV2L_RPC_CACHE_LINES];
    uint8_t cache_data[RENESAS_RZV2L_RPC_CACHE_LINES]
                      [RENESAS_RZV2L_RPC_LINE_SIZE];
};

#endif
