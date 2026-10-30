/*
 * Renesas RZ/G2L family SD/MMC host interface (SDHI)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * One channel of the SD/MMC host interface of the Renesas RZ/G2L family
 * (RZ/G2L, RZ/V2L), documented in the RZ/V2L Group User's Manual:
 * Hardware (R01UH0936EJ), section 40.
 *
 * QEMU interface:
 *  + QOM property "has-sd-status": the channel has SD_STATUS (channel 0)
 *  + QOM properties "cd-connected", "wp-connected": the board routes the
 *    card slot's card detect / write protect switch to the ISDCD / ISDWP
 *    input (default on); an unrouted input stays high, as the manual
 *    asks for unused SDx_CD and SDx_WP pins
 *  + QOM link "dma-memory": the memory the built-in DMAC reads and writes
 *  + sysbus MMIO region 0: the channel's registers (64 KiB)
 *  + sysbus IRQ 0: OXMNIRQ, card access, card detect and DMAC interrupts
 *  + sysbus IRQ 1: OXASIOIRQ, SDIO interrupts
 *  + named GPIO input "ixrst": high while the clock controller holds the
 *    channel in reset (its IXRST line is low)
 *  + child bus "sd-bus": the card
 */

#ifndef HW_SD_RENESAS_SDHI_H
#define HW_SD_RENESAS_SDHI_H

#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_RENESAS_SDHI "renesas-rzg2l-sdhi"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasSDHIState, RENESAS_SDHI)

#define TYPE_RENESAS_SDHI_BUS "renesas-rzg2l-sdhi-bus"

#define RENESAS_SDHI_MMIO_SIZE  0x10000
#define RENESAS_SDHI_BLOCK_MAX  512

struct RenesasSDHIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq;
    qemu_irq irq_sdio;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;

    bool in_reset;

    /* SD card interface registers */
    uint32_t sd_cmd;
    uint32_t sd_arg;
    uint32_t sd_stop;
    uint32_t sd_seccnt;
    uint64_t sd_rsp10;
    uint64_t sd_rsp54;
    uint32_t sd_info1;
    uint32_t sd_info2;
    uint32_t sd_info1_mask;
    uint32_t sd_info2_mask;
    uint32_t sd_clk_ctrl;
    uint32_t sd_size;
    uint32_t sd_option;
    uint32_t sd_err_sts1;
    uint32_t sd_err_sts2;
    uint32_t sdio_mode;
    uint32_t sdio_info1;
    uint32_t sdio_info1_mask;
    uint32_t cc_ext_mode;
    uint32_t soft_rst;
    uint32_t host_mode;
    uint32_t sdif_mode;
    uint32_t sd_status;

    /* Built-in DMAC registers */
    uint32_t dm_dtran_mode;
    uint32_t dm_dtran_ctrl;
    uint32_t dm_rst;
    uint32_t dm_info1;
    uint32_t dm_info1_mask;
    uint32_t dm_info2;
    uint32_t dm_info2_mask;
    uint32_t dm_dtran_addr;

    /* The command sequence in progress (SD_INFO2.CBSY) */
    bool busy;
    bool data_read;
    bool data_write;
    bool auto_cmd12;
    bool multi;
    bool dma_wait;
    uint32_t blocks_left;
    uint32_t buf_len;
    uint32_t buf_pos;
    uint8_t buf[RENESAS_SDHI_BLOCK_MAX];

    /* Properties */
    bool has_sd_status;
    bool cd_connected;
    bool wp_connected;
};

#endif
