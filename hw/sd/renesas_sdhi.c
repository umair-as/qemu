/*
 * Renesas RZ/G2L family SD/MMC host interface (SDHI)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * One channel of the SD/MMC host interface, RZ/V2L Group User's Manual:
 * Hardware, section 40. Registers sit 8 bytes apart (Table 40.2) and
 * take 16-, 32- and 64-bit accesses; a narrower access reads or writes
 * part of the register.
 *
 * - Writing SD_CMD starts a command sequence on the SD bus: SD_INFO2.CBSY
 *   is 1 until it ends. The response goes to SD_RSP10 and SD_RSP54
 *   (Table 40.3) and sets SD_INFO1.INFO0. A command the card does not
 *   answer ends with a response timeout (SD_INFO2.ERR6).
 * - A command with data moves SD_SIZE bytes per block: one block, or
 *   SD_SECCNT blocks when SD_STOP.SEC is set. A multiple block transfer
 *   ends with an automatic CMD12 unless SD_CMD.MD6 is set. The end of the
 *   data sets SD_INFO1.INFO2 (access end) and the sequence is over.
 * - Without DMA (CC_EXT_MODE.DMASDRW = 0), the data goes through
 *   SD_BUF0, one block at a time: SD_INFO2.BRE or BWE is set while a
 *   block can be read or written. Each access moves as many bytes as it
 *   is wide; HOST_MODE is stored only.
 * - With DMA, the built-in DMAC moves every block between the card and
 *   DM_DTRAN_ADDR once DM_CM_DTRAN_CTRL.DM_START is written after the
 *   response (Figures 40.22 and 40.23), then sets DTRANEND0 (to the
 *   card) or DTRANEND1 (from the card) in DM_CM_INFO1.
 * - Flags in SD_INFO1, SD_INFO2, SDIO_INFO1 and DM_CM_INFO1/2 clear when
 *   0 is written to them; writing 1 leaves them. OXMNIRQ is high while a
 *   card access, card detect or DMAC flag is set and not masked, as is
 *   OXASIOIRQ for the SDIO flags (Table 40.5).
 *
 * The SD bus has no timing: a sequence ends within the register access
 * that starts it, and the data timeout never fires. Not modelled: SDIO
 * card functions, the HPI and SDIO abort commands, the read wait state,
 * the SD_BUF0 byte swap (HOST_MODE.ENDIAN), CRC errors, and the sampling
 * clock controller (SCC) at offset H'1000: its registers read as zero
 * and are logged, so tuning fails visibly. While the clock controller
 * holds the channel in reset, its registers read their reset values and
 * writes are ignored. Offsets the manual does not list read as zero and
 * are logged.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "system/dma.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/sd/renesas_sdhi.h"
#include "migration/vmstate.h"
#include "trace.h"

REG32(SD_CMD, 0x000)
    FIELD(SD_CMD, INDEX, 0, 6)
    FIELD(SD_CMD, ACMD, 6, 2)
    FIELD(SD_CMD, MODE, 8, 3)
    FIELD(SD_CMD, DATA, 11, 1)
    FIELD(SD_CMD, READ, 12, 1)
    FIELD(SD_CMD, MULTI, 13, 1)
    FIELD(SD_CMD, NO_CMD12, 14, 1)
REG32(SD_ARG, 0x010)
REG32(SD_ARG1, 0x018)
REG32(SD_STOP, 0x020)
    FIELD(SD_STOP, STP, 0, 1)
    FIELD(SD_STOP, SEC, 8, 1)
REG32(SD_SECCNT, 0x028)
REG32(SD_RSP10, 0x030)
REG32(SD_RSP1, 0x038)
REG32(SD_RSP32, 0x040)
REG32(SD_RSP3, 0x048)
REG32(SD_RSP54, 0x050)
REG32(SD_RSP5, 0x058)
REG32(SD_RSP76, 0x060)
REG32(SD_RSP7, 0x068)
REG32(SD_INFO1, 0x070)
    FIELD(SD_INFO1, INFO0, 0, 1)
    FIELD(SD_INFO1, INFO2, 2, 1)
    FIELD(SD_INFO1, INFO3, 3, 1)
    FIELD(SD_INFO1, INFO4, 4, 1)
    FIELD(SD_INFO1, INFO5, 5, 1)
    FIELD(SD_INFO1, INFO7, 7, 1)
    FIELD(SD_INFO1, INFO10, 10, 1)
REG32(SD_INFO2, 0x078)
    FIELD(SD_INFO2, ERR0, 0, 1)
    FIELD(SD_INFO2, ERR3, 3, 1)
    FIELD(SD_INFO2, ERR4, 4, 1)
    FIELD(SD_INFO2, ERR5, 5, 1)
    FIELD(SD_INFO2, ERR6, 6, 1)
    FIELD(SD_INFO2, DAT0, 7, 1)
    FIELD(SD_INFO2, BRE, 8, 1)
    FIELD(SD_INFO2, BWE, 9, 1)
    FIELD(SD_INFO2, SCLKDIVEN, 13, 1)
    FIELD(SD_INFO2, CBSY, 14, 1)
    FIELD(SD_INFO2, ILA, 15, 1)
REG32(SD_INFO1_MASK, 0x080)
REG32(SD_INFO2_MASK, 0x088)
REG32(SD_CLK_CTRL, 0x090)
    FIELD(SD_CLK_CTRL, SCLKEN, 8, 1)
REG32(SD_SIZE, 0x098)
REG32(SD_OPTION, 0x0a0)
REG32(SD_ERR_STS1, 0x0b0)
REG32(SD_ERR_STS2, 0x0b8)
    FIELD(SD_ERR_STS2, E0, 0, 1)
    FIELD(SD_ERR_STS2, E1, 1, 1)
    FIELD(SD_ERR_STS2, E4, 4, 1)
REG32(SD_BUF0, 0x0c0)
REG32(SDIO_MODE, 0x0d0)
REG32(SDIO_INFO1, 0x0d8)
REG32(SDIO_INFO1_MASK, 0x0e0)
REG32(CC_EXT_MODE, 0x360)
    FIELD(CC_EXT_MODE, DMASDRW, 1, 1)
REG32(SOFT_RST, 0x380)
    FIELD(SOFT_RST, SDRST, 0, 1)
REG32(VERSION, 0x388)
REG32(HOST_MODE, 0x390)
    FIELD(HOST_MODE, ENDIAN, 1, 1)
REG32(SDIF_MODE, 0x398)
REG32(SD_STATUS, 0x3c8)
REG32(DM_CM_DTRAN_MODE, 0x820)
    FIELD(DM_CM_DTRAN_MODE, BUS_WIDTH, 4, 2)
    FIELD(DM_CM_DTRAN_MODE, CH_NUM, 16, 2)
REG32(DM_CM_DTRAN_CTRL, 0x828)
    FIELD(DM_CM_DTRAN_CTRL, DM_START, 0, 1)
REG32(DM_CM_RST, 0x830)
    FIELD(DM_CM_RST, SEQRST, 0, 1)
    FIELD(DM_CM_RST, DTRANRST0, 8, 1)
    FIELD(DM_CM_RST, DTRANRST1, 9, 1)
REG32(DM_CM_INFO1, 0x840)
    FIELD(DM_CM_INFO1, DTRANEND0, 16, 1)
    FIELD(DM_CM_INFO1, DTRANEND1, 20, 1)
REG32(DM_CM_INFO1_MASK, 0x848)
REG32(DM_CM_INFO2, 0x850)
    FIELD(DM_CM_INFO2, DTRANERR0, 16, 1)
    FIELD(DM_CM_INFO2, DTRANERR1, 17, 1)
REG32(DM_CM_INFO2_MASK, 0x858)
REG32(DM_DTRAN_ADDR, 0x880)

#define SCC_BASE            0x1000
#define SCC_END             0x1040

/* SD_CMD[10:8] (section 40.2.1) */
#define MODE_NORMAL         0
#define MODE_NO_RSP         3
#define MODE_R1             4
#define MODE_R1B            5
#define MODE_R2             6
#define MODE_R3             7

/* Bits that clear on a 0 write; the others of these registers are read-only */
#define SD_INFO1_W0C        0x0001031d
#define SD_INFO2_W0C        0x0000837f
#define SDIO_INFO1_W0C      0x0000c001
#define DM_CM_INFO1_W0C     0x00110001
#define DM_CM_INFO2_W0C     0x00030001

/* Reset values (section 40.2) */
#define SD_INFO1_MASK_RESET     0x0001031d
#define SD_INFO2_MASK_RESET     0x00008b7f
#define SD_CLK_CTRL_RESET       0x00000020
#define SD_SIZE_RESET           0x00000200
#define SD_OPTION_RESET         0x000040ee
#define SD_ERR_STS1_RESET       0x00002000
#define SDIO_INFO1_MASK_RESET   0x0000c007
#define CC_EXT_MODE_RESET       0x00001010
#define SOFT_RST_RESET          0x00000007
#define VERSION_VALUE           0x0000cc10
#define DM_CM_DTRAN_MODE_RESET  0x00000030

/* Writable bits; the rest of each register is fixed */
#define SD_STOP_WMASK           0x00070101
#define SD_INFO1_MASK_WMASK     0x0001031d
#define SD_INFO2_MASK_WMASK     0x00008b7f
#define SD_CLK_CTRL_WMASK       0x000107ff
#define SD_SIZE_WMASK           0x000003ff
#define SD_OPTION_WMASK         0x0000a3ff
#define SDIO_MODE_WMASK         0x00000305
#define SDIO_INFO1_MASK_WMASK   0x0000c007
#define CC_EXT_MODE_WMASK       0x00000323
#define HOST_MODE_WMASK         0x00000103
#define SDIF_MODE_WMASK         0x00000300
#define SD_STATUS_WMASK         0x00000002
#define DM_CM_DTRAN_MODE_WMASK  0x00030030
#define DM_CM_RST_WMASK         0x00000301
#define DM_CM_INFO1_MASK_WMASK  0x001f0001
#define DM_CM_INFO2_MASK_WMASK  0x00030001
#define DM_DTRAN_ADDR_WMASK     0xfffffff8

/* The flags each interrupt mask covers (Table 40.5) */
#define SD_INFO1_IRQS       0x0001031d
#define SD_INFO2_IRQS       0x0000837f
#define SDIO_INFO1_IRQS     0x0000c001
#define DM_CM_INFO1_IRQS    0x00110001
#define DM_CM_INFO2_IRQS    0x00030001

static void sdhi_update_irq(RenesasSDHIState *s)
{
    bool sdio = s->sdio_info1 & ~s->sdio_info1_mask & SDIO_INFO1_IRQS;
    bool main = (s->sd_info1 & ~s->sd_info1_mask & SD_INFO1_IRQS) ||
                (s->sd_info2 & ~s->sd_info2_mask & SD_INFO2_IRQS) ||
                (s->dm_info1 & ~s->dm_info1_mask & DM_CM_INFO1_IRQS) ||
                (s->dm_info2 & ~s->dm_info2_mask & DM_CM_INFO2_IRQS);

    trace_renesas_sdhi_irq(main || sdio, sdio);
    qemu_set_irq(s->irq, main || sdio);
    qemu_set_irq(s->irq_sdio, sdio);
}

/* The registers SOFT_RST.SDRST = 0 returns to their reset values */
static void sdhi_reset_sd_unit(RenesasSDHIState *s)
{
    s->sd_stop = 0;
    s->sd_info1 &= ~SD_INFO1_W0C;
    s->sd_info2 &= ~SD_INFO2_W0C;
    s->sd_clk_ctrl &= ~(R_SD_CLK_CTRL_SCLKEN_MASK | BIT(10));
    s->sd_option = SD_OPTION_RESET;
    s->sd_err_sts1 = SD_ERR_STS1_RESET;
    s->sd_err_sts2 = 0;
    s->sdio_info1 = 0;
    s->sdif_mode = 0;
    s->busy = false;
    s->data_read = false;
    s->data_write = false;
    s->dma_wait = false;
}

static void sdhi_reset_state(RenesasSDHIState *s)
{
    s->sd_cmd = 0;
    s->sd_arg = 0;
    s->sd_seccnt = 0;
    s->sd_rsp10 = 0;
    s->sd_rsp54 = 0;
    s->sd_info1 = 0;
    s->sd_info2 = 0;
    s->sd_info1_mask = SD_INFO1_MASK_RESET;
    s->sd_info2_mask = SD_INFO2_MASK_RESET;
    s->sd_clk_ctrl = SD_CLK_CTRL_RESET;
    s->sd_size = SD_SIZE_RESET;
    s->sdio_mode = 0;
    s->sdio_info1_mask = SDIO_INFO1_MASK_RESET;
    s->cc_ext_mode = CC_EXT_MODE_RESET;
    s->soft_rst = SOFT_RST_RESET;
    s->host_mode = 0;
    s->sd_status = 0;
    s->dm_dtran_mode = DM_CM_DTRAN_MODE_RESET;
    s->dm_dtran_ctrl = 0;
    s->dm_rst = 0xffffffff;
    s->dm_info1 = 0;
    s->dm_info1_mask = 0xffffffff;
    s->dm_info2 = 0;
    s->dm_info2_mask = 0xffffffff;
    s->dm_dtran_addr = 0;
    s->auto_cmd12 = false;
    s->multi = false;
    s->blocks_left = 0;
    s->buf_len = 0;
    s->buf_pos = 0;
    sdhi_reset_sd_unit(s);
}

/* The card's pins, seen in SD_INFO1 and SD_INFO2 */
static uint32_t sdhi_info1(RenesasSDHIState *s)
{
    uint32_t v = s->sd_info1;

    if (sdbus_get_inserted(&s->sdbus)) {
        v |= R_SD_INFO1_INFO5_MASK | R_SD_INFO1_INFO10_MASK;
        if (!sdbus_get_readonly(&s->sdbus)) {
            v |= R_SD_INFO1_INFO7_MASK;
        }
    }
    return v;
}

static uint32_t sdhi_info2(RenesasSDHIState *s)
{
    uint32_t v = s->sd_info2 | R_SD_INFO2_DAT0_MASK;

    if (s->busy) {
        v |= R_SD_INFO2_CBSY_MASK;
    } else {
        v |= R_SD_INFO2_SCLKDIVEN_MASK;
    }
    return v;
}

/* End of a command sequence: access end (with data) or response end */
static void sdhi_end_sequence(RenesasSDHIState *s, bool access_end)
{
    if (access_end) {
        s->sd_info1 |= R_SD_INFO1_INFO2_MASK;
    }
    s->busy = false;
    s->data_read = false;
    s->data_write = false;
    s->dma_wait = false;
    trace_renesas_sdhi_end(access_end, s->sd_info2);
}

/*
 * Send one command to the card. @rtype is the SD_CMD response type, or
 * MODE_NORMAL to take the response the card gives. A command the card
 * does not answer, or answers with the wrong length, times out.
 */
static bool sdhi_send(RenesasSDHIState *s, uint8_t index, uint32_t arg,
                      int rtype, bool in_sequence)
{
    SDRequest req = { .cmd = index, .arg = arg };
    uint8_t rsp[16];
    size_t rlen;
    bool ok;

    rlen = sdbus_do_command(&s->sdbus, &req, rsp, sizeof(rsp));
    trace_renesas_sdhi_command(index, arg, rtype, rlen);

    switch (rtype) {
    case MODE_NO_RSP:
        return true;
    case MODE_R2:
        ok = rlen == 16;
        break;
    case MODE_NORMAL:
        ok = rlen == 4 || rlen == 16 || rlen == 0;
        break;
    default:
        ok = rlen == 4;
        break;
    }
    if (!ok || (rtype != MODE_NORMAL && rlen == 0)) {
        s->sd_info2 |= R_SD_INFO2_ERR6_MASK;
        s->sd_err_sts2 |= in_sequence ? R_SD_ERR_STS2_E1_MASK
                                      : R_SD_ERR_STS2_E0_MASK;
        return false;
    }
    if (rlen == 16) {
        /* R2: R[127:8] in SD_RSP54 (R[127:72]) and SD_RSP10 (R[71:8]) */
        s->sd_rsp54 = ldq_be_p(rsp) >> 8;
        s->sd_rsp10 = ldq_be_p(rsp + 7);
    } else if (rlen == 4) {
        s->sd_rsp10 = ldl_be_p(rsp);
    }
    return true;
}

/* The automatic CMD12 after the last block of a multiple block transfer */
static void sdhi_finish_data(RenesasSDHIState *s)
{
    if (s->auto_cmd12) {
        sdhi_send(s, 12, 0, MODE_R1B, true);
    }
    sdhi_end_sequence(s, true);
}

/* The next block for SD_BUF0, or the end of the data */
static void sdhi_pio_next(RenesasSDHIState *s)
{
    s->buf_pos = 0;
    if (!s->blocks_left) {
        sdhi_finish_data(s);
        return;
    }
    if (s->data_read) {
        if (!sdbus_data_ready(&s->sdbus)) {
            /* The card sends nothing: data timeout (section 40.2.7) */
            s->sd_info2 |= R_SD_INFO2_ERR3_MASK;
            s->sd_err_sts2 |= R_SD_ERR_STS2_E4_MASK;
            sdhi_end_sequence(s, true);
            return;
        }
        sdbus_read_data(&s->sdbus, s->buf, s->buf_len);
        s->sd_info2 |= R_SD_INFO2_BRE_MASK;
    } else {
        s->sd_info2 |= R_SD_INFO2_BWE_MASK;
    }
}

static void sdhi_start_command(RenesasSDHIState *s)
{
    uint32_t cmd = s->sd_cmd;
    uint8_t index = FIELD_EX32(cmd, SD_CMD, INDEX);
    int mode = FIELD_EX32(cmd, SD_CMD, MODE);
    bool data, read, multi, no_cmd12;

    if (mode == 1 || mode == 2 || (cmd & BIT(15)) ||
        FIELD_EX32(cmd, SD_CMD, ACMD) > 1) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: prohibited SD_CMD 0x%04x\n",
                      __func__, cmd);
        return;
    }
    if (mode == MODE_NORMAL) {
        /* Normal mode: only the block read and write commands carry data */
        data = index == 17 || index == 18 || index == 24 || index == 25;
        read = index == 17 || index == 18;
        multi = index == 18 || index == 25;
        no_cmd12 = false;
        qemu_log_mask(LOG_UNIMP, "%s: SD_CMD normal mode, CMD%u\n",
                      __func__, index);
    } else {
        data = FIELD_EX32(cmd, SD_CMD, DATA);
        read = FIELD_EX32(cmd, SD_CMD, READ);
        multi = FIELD_EX32(cmd, SD_CMD, MULTI);
        no_cmd12 = FIELD_EX32(cmd, SD_CMD, NO_CMD12);
    }
    if (data && index == 12) {
        s->sd_info2 |= R_SD_INFO2_ILA_MASK;
        return;
    }
    if (!(s->sd_clk_ctrl & R_SD_CLK_CTRL_SCLKEN_MASK)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: CMD%u with the SD clock stopped (SCLKEN = 0)\n",
                      __func__, index);
    }

    s->busy = true;
    if (!sdhi_send(s, index, s->sd_arg, mode, false)) {
        /* Halted by the response timeout: INFO0, and INFO2 with data */
        s->sd_info1 |= R_SD_INFO1_INFO0_MASK;
        sdhi_end_sequence(s, data);
        return;
    }
    s->sd_info1 |= R_SD_INFO1_INFO0_MASK;
    if (!data) {
        sdhi_end_sequence(s, false);
        return;
    }

    if (multi && (index == 18 || index == 25)) {
        /* The response to CMD18 and CMD25 is kept in R[103:72] too */
        s->sd_rsp54 = (s->sd_rsp54 & ~0xffffffffull) | (uint32_t)s->sd_rsp10;
    }
    s->multi = multi;
    s->data_read = read;
    s->data_write = !read;
    s->buf_len = s->sd_size ? s->sd_size : RENESAS_SDHI_BLOCK_MAX;
    if (!multi) {
        s->blocks_left = 1;
    } else if (s->sd_stop & R_SD_STOP_SEC_MASK) {
        s->blocks_left = s->sd_seccnt;
    } else {
        /* Without SEC the transfer runs until SD_STOP.STP */
        s->blocks_left = UINT32_MAX;
    }
    s->auto_cmd12 = multi && (s->sd_stop & R_SD_STOP_SEC_MASK) && !no_cmd12;

    if (s->cc_ext_mode & R_CC_EXT_MODE_DMASDRW_MASK) {
        /* The DMAC moves the data once DM_START is written */
        s->dma_wait = true;
    } else {
        sdhi_pio_next(s);
    }
}

/* The DMAC moves every remaining block between the card and memory */
static void sdhi_dma_run(RenesasSDHIState *s)
{
    bool to_memory = FIELD_EX32(s->dm_dtran_mode, DM_CM_DTRAN_MODE, CH_NUM);
    uint32_t end = to_memory ? R_DM_CM_INFO1_DTRANEND1_MASK
                             : R_DM_CM_INFO1_DTRANEND0_MASK;
    uint32_t err = to_memory ? R_DM_CM_INFO2_DTRANERR1_MASK
                             : R_DM_CM_INFO2_DTRANERR0_MASK;
    uint64_t addr = s->dm_dtran_addr;
    MemTxResult res = MEMTX_OK;

    if (!s->dma_wait) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DM_START without a data transfer waiting\n",
                      __func__);
        return;
    }
    if (FIELD_EX32(s->dm_dtran_mode, DM_CM_DTRAN_MODE, CH_NUM) > 1 ||
        FIELD_EX32(s->dm_dtran_mode, DM_CM_DTRAN_MODE, BUS_WIDTH) != 3 ||
        to_memory != s->data_read) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DM_CM_DTRAN_MODE 0x%x does not fit the transfer\n",
                      __func__, s->dm_dtran_mode);
        s->dm_info1 |= end;
        s->dm_info2 |= err;
        return;
    }

    s->dma_wait = false;
    trace_renesas_sdhi_dma(to_memory, addr, s->blocks_left, s->buf_len);
    while (s->blocks_left && res == MEMTX_OK) {
        if (addr + s->buf_len > (1ull << 32)) {
            res = MEMTX_DECODE_ERROR;
            break;
        }
        if (to_memory) {
            if (!sdbus_data_ready(&s->sdbus)) {
                s->sd_info2 |= R_SD_INFO2_ERR3_MASK;
                s->sd_err_sts2 |= R_SD_ERR_STS2_E4_MASK;
                break;
            }
            sdbus_read_data(&s->sdbus, s->buf, s->buf_len);
            res = dma_memory_write(&s->dma_as, addr, s->buf, s->buf_len,
                                   MEMTXATTRS_UNSPECIFIED);
        } else {
            res = dma_memory_read(&s->dma_as, addr, s->buf, s->buf_len,
                                  MEMTXATTRS_UNSPECIFIED);
            if (res == MEMTX_OK) {
                sdbus_write_data(&s->sdbus, s->buf, s->buf_len);
            }
        }
        addr += s->buf_len;
        s->blocks_left--;
    }
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DMA access to 0x%" PRIx64 " failed\n",
                      __func__, addr);
        s->dm_info2 |= err;
        sdhi_end_sequence(s, true);
    } else if (s->sd_info2 & R_SD_INFO2_ERR3_MASK) {
        sdhi_end_sequence(s, true);
    } else {
        sdhi_finish_data(s);
    }
    s->dm_info1 |= end;
}

/* SD_STOP.STP: stop the data transfer in progress */
static void sdhi_stop(RenesasSDHIState *s)
{
    if (!s->data_read && !s->data_write) {
        return;
    }
    if (s->multi) {
        sdhi_send(s, 12, 0, MODE_R1B, true);
    }
    sdhi_end_sequence(s, true);
}

static uint64_t sdhi_buf_read(RenesasSDHIState *s, unsigned size)
{
    uint64_t v = 0;

    if (!s->data_read || s->dma_wait || s->buf_pos >= s->buf_len) {
        s->sd_info2 |= R_SD_INFO2_ERR5_MASK;
        return 0;
    }
    memcpy(&v, s->buf + s->buf_pos, MIN(size, s->buf_len - s->buf_pos));
    s->buf_pos += size;
    if (s->buf_pos >= s->buf_len) {
        s->blocks_left--;
        sdhi_pio_next(s);
    }
    return le64_to_cpu(v);
}

static void sdhi_buf_write(RenesasSDHIState *s, uint64_t value, unsigned size)
{
    uint64_t v = cpu_to_le64(value);

    if (!s->data_write || s->dma_wait || s->buf_pos >= s->buf_len) {
        s->sd_info2 |= R_SD_INFO2_ERR4_MASK;
        return;
    }
    memcpy(s->buf + s->buf_pos, &v, MIN(size, s->buf_len - s->buf_pos));
    s->buf_pos += size;
    if (s->buf_pos >= s->buf_len) {
        sdbus_write_data(&s->sdbus, s->buf, s->buf_len);
        s->blocks_left--;
        sdhi_pio_next(s);
    }
}

/* The value of the 8-byte register slot at @reg */
static uint64_t sdhi_reg_read(RenesasSDHIState *s, hwaddr reg)
{
    switch (reg) {
    case A_SD_CMD:
        return s->sd_cmd;
    case A_SD_ARG:
        return s->sd_arg;
    case A_SD_ARG1:
        return s->sd_arg >> 16;
    case A_SD_STOP:
        return s->sd_stop;
    case A_SD_SECCNT:
        return s->sd_seccnt;
    case A_SD_RSP10:
        return s->sd_rsp10;
    case A_SD_RSP1:
        return extract64(s->sd_rsp10, 16, 16);
    case A_SD_RSP32:
        return extract64(s->sd_rsp10, 32, 32);
    case A_SD_RSP3:
        return extract64(s->sd_rsp10, 48, 16);
    case A_SD_RSP54:
        return s->sd_rsp54;
    case A_SD_RSP5:
        return extract64(s->sd_rsp54, 16, 16);
    case A_SD_RSP76:
        return extract64(s->sd_rsp54, 32, 24);
    case A_SD_RSP7:
        return extract64(s->sd_rsp54, 48, 8);
    case A_SD_INFO1:
        return sdhi_info1(s);
    case A_SD_INFO2:
        return sdhi_info2(s);
    case A_SD_INFO1_MASK:
        return s->sd_info1_mask;
    case A_SD_INFO2_MASK:
        return s->sd_info2_mask;
    case A_SD_CLK_CTRL:
        return s->sd_clk_ctrl;
    case A_SD_SIZE:
        return s->sd_size;
    case A_SD_OPTION:
        return s->sd_option;
    case A_SD_ERR_STS1:
        return s->sd_err_sts1;
    case A_SD_ERR_STS2:
        return s->sd_err_sts2;
    case A_SDIO_MODE:
        return s->sdio_mode;
    case A_SDIO_INFO1:
        return s->sdio_info1;
    case A_SDIO_INFO1_MASK:
        return s->sdio_info1_mask;
    case A_CC_EXT_MODE:
        return s->cc_ext_mode;
    case A_SOFT_RST:
        return s->soft_rst;
    case A_VERSION:
        return VERSION_VALUE;
    case A_HOST_MODE:
        return s->host_mode;
    case A_SDIF_MODE:
        return s->sdif_mode;
    case A_DM_CM_DTRAN_MODE:
        return s->dm_dtran_mode;
    case A_DM_CM_DTRAN_CTRL:
        return s->dm_dtran_ctrl;
    case A_DM_CM_RST:
        return s->dm_rst;
    case A_DM_CM_INFO1:
        return s->dm_info1;
    case A_DM_CM_INFO1_MASK:
        return s->dm_info1_mask;
    case A_DM_CM_INFO2:
        return s->dm_info2;
    case A_DM_CM_INFO2_MASK:
        return s->dm_info2_mask;
    case A_DM_DTRAN_ADDR:
        return s->dm_dtran_addr;
    case A_SD_STATUS:
        return s->sd_status;
    default:
        return 0;
    }
}

/* Whether the manual lists a register at @reg (Table 40.2) */
static bool sdhi_reg_known(RenesasSDHIState *s, hwaddr reg)
{
    switch (reg) {
    case A_SD_CMD ... A_SD_SECCNT:
        return reg != 0x008;
    case A_SD_RSP10 ... A_SD_OPTION:
    case A_SD_ERR_STS1 ... A_SD_BUF0:
    case A_SDIO_MODE ... A_SDIO_INFO1_MASK:
    case A_CC_EXT_MODE:
    case A_SOFT_RST ... A_SDIF_MODE:
    case A_DM_CM_DTRAN_MODE ... A_DM_CM_RST:
    case A_DM_CM_INFO1 ... A_DM_CM_INFO2_MASK:
    case A_DM_DTRAN_ADDR:
        return true;
    case A_SD_STATUS:
        return s->has_sd_status;
    default:
        return false;
    }
}

static uint32_t sdhi_wmask(uint32_t old, uint32_t value, uint32_t wmask)
{
    return (old & ~wmask) | (value & wmask);
}

/* A flag register: bits written 0 clear, bits written 1 stay */
static uint32_t sdhi_w0c(uint32_t old, uint32_t value, uint32_t w0c)
{
    return old & (value | ~w0c);
}

static void sdhi_reg_write(RenesasSDHIState *s, hwaddr reg, uint64_t value)
{
    uint32_t v = value;

    switch (reg) {
    case A_SD_CMD:
        if (s->busy) {
            s->sd_info2 |= R_SD_INFO2_ILA_MASK;
            break;
        }
        if (!(s->soft_rst & R_SOFT_RST_SDRST_MASK)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: SD_CMD written while SOFT_RST holds the reset\n",
                          __func__);
            break;
        }
        s->sd_cmd = v & 0xffff;
        sdhi_start_command(s);
        break;
    case A_SD_ARG:
        s->sd_arg = v;
        break;
    case A_SD_ARG1:
        s->sd_arg = deposit32(s->sd_arg, 16, 16, v);
        break;
    case A_SD_STOP:
        if (v & BIT(16)) {
            qemu_log_mask(LOG_UNIMP, "%s: HPI command\n", __func__);
        }
        s->sd_stop = sdhi_wmask(s->sd_stop, v, SD_STOP_WMASK) & ~BIT(16);
        if (v & R_SD_STOP_STP_MASK) {
            sdhi_stop(s);
        }
        break;
    case A_SD_SECCNT:
        s->sd_seccnt = v;
        break;
    case A_SD_INFO1:
        s->sd_info1 = sdhi_w0c(s->sd_info1, v, SD_INFO1_W0C);
        break;
    case A_SD_INFO2:
        s->sd_info2 = sdhi_w0c(s->sd_info2, v, SD_INFO2_W0C);
        /* Bit 11 is a plain read/write bit, to be written 1 */
        s->sd_info2 = deposit32(s->sd_info2, 11, 1, v >> 11);
        break;
    case A_SD_INFO1_MASK:
        s->sd_info1_mask = sdhi_wmask(s->sd_info1_mask, v,
                                      SD_INFO1_MASK_WMASK);
        break;
    case A_SD_INFO2_MASK:
        s->sd_info2_mask = sdhi_wmask(s->sd_info2_mask, v,
                                      SD_INFO2_MASK_WMASK);
        break;
    case A_SD_CLK_CTRL:
        if (s->busy) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: SD_CLK_CTRL written while CBSY = 1\n",
                          __func__);
            break;
        }
        s->sd_clk_ctrl = sdhi_wmask(s->sd_clk_ctrl, v, SD_CLK_CTRL_WMASK);
        break;
    case A_SD_SIZE:
        s->sd_size = sdhi_wmask(s->sd_size, v, SD_SIZE_WMASK);
        if (s->sd_size > RENESAS_SDHI_BLOCK_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: SD_SIZE %u above 512\n",
                          __func__, s->sd_size);
            s->sd_size = RENESAS_SDHI_BLOCK_MAX;
        }
        break;
    case A_SD_OPTION:
        s->sd_option = sdhi_wmask(s->sd_option, v, SD_OPTION_WMASK);
        break;
    case A_SD_BUF0:
        /* Handled by the caller, which knows the access size */
        break;
    case A_SDIO_MODE:
        if (v & (BIT(9) | BIT(8) | BIT(2))) {
            qemu_log_mask(LOG_UNIMP, "%s: SDIO_MODE 0x%x\n", __func__, v);
        }
        s->sdio_mode = sdhi_wmask(s->sdio_mode, v, SDIO_MODE_WMASK);
        break;
    case A_SDIO_INFO1:
        s->sdio_info1 = sdhi_w0c(s->sdio_info1, v, SDIO_INFO1_W0C);
        break;
    case A_SDIO_INFO1_MASK:
        s->sdio_info1_mask = sdhi_wmask(s->sdio_info1_mask, v,
                                        SDIO_INFO1_MASK_WMASK);
        break;
    case A_CC_EXT_MODE:
        s->cc_ext_mode = sdhi_wmask(s->cc_ext_mode, v, CC_EXT_MODE_WMASK);
        break;
    case A_SOFT_RST:
        s->soft_rst = sdhi_wmask(s->soft_rst, v, R_SOFT_RST_SDRST_MASK);
        if (!(s->soft_rst & R_SOFT_RST_SDRST_MASK)) {
            sdhi_reset_sd_unit(s);
        }
        break;
    case A_HOST_MODE:
        if (v & R_HOST_MODE_ENDIAN_MASK) {
            qemu_log_mask(LOG_UNIMP, "%s: SD_BUF0 byte swap\n", __func__);
        }
        s->host_mode = sdhi_wmask(s->host_mode, v, HOST_MODE_WMASK);
        break;
    case A_SDIF_MODE:
        s->sdif_mode = sdhi_wmask(s->sdif_mode, v, SDIF_MODE_WMASK);
        break;
    case A_DM_CM_DTRAN_MODE:
        s->dm_dtran_mode = sdhi_wmask(s->dm_dtran_mode, v,
                                      DM_CM_DTRAN_MODE_WMASK);
        break;
    case A_DM_CM_DTRAN_CTRL:
        /* DM_START clears itself when the transfer starts */
        s->dm_dtran_ctrl = v & BIT(8);
        if (v & R_DM_CM_DTRAN_CTRL_DM_START_MASK) {
            sdhi_dma_run(s);
        }
        break;
    case A_DM_CM_RST:
        s->dm_rst = sdhi_wmask(s->dm_rst, v, DM_CM_RST_WMASK);
        if (!(s->dm_rst & R_DM_CM_RST_SEQRST_MASK)) {
            s->dm_info1 &= ~BIT(0);
            s->dm_info2 &= ~BIT(0);
        }
        if (!(s->dm_rst & R_DM_CM_RST_DTRANRST0_MASK)) {
            s->dm_info1 &= ~R_DM_CM_INFO1_DTRANEND0_MASK;
            s->dm_info2 &= ~R_DM_CM_INFO2_DTRANERR0_MASK;
        }
        if (!(s->dm_rst & R_DM_CM_RST_DTRANRST1_MASK)) {
            s->dm_info1 &= ~R_DM_CM_INFO1_DTRANEND1_MASK;
            s->dm_info2 &= ~R_DM_CM_INFO2_DTRANERR1_MASK;
        }
        break;
    case A_DM_CM_INFO1:
        s->dm_info1 = sdhi_w0c(s->dm_info1, v, DM_CM_INFO1_W0C);
        break;
    case A_DM_CM_INFO1_MASK:
        s->dm_info1_mask = sdhi_wmask(s->dm_info1_mask, v,
                                      DM_CM_INFO1_MASK_WMASK);
        break;
    case A_DM_CM_INFO2:
        s->dm_info2 = sdhi_w0c(s->dm_info2, v, DM_CM_INFO2_W0C);
        break;
    case A_DM_CM_INFO2_MASK:
        s->dm_info2_mask = sdhi_wmask(s->dm_info2_mask, v,
                                      DM_CM_INFO2_MASK_WMASK);
        break;
    case A_DM_DTRAN_ADDR:
        s->dm_dtran_addr = v & DM_DTRAN_ADDR_WMASK;
        break;
    case A_SD_RSP10 ... A_SD_RSP7:
    case A_SD_ERR_STS1:
    case A_SD_ERR_STS2:
    case A_VERSION:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only offset 0x%"
                      HWADDR_PRIx "\n", __func__, reg);
        break;
    case A_SD_STATUS:
        s->sd_status = sdhi_wmask(s->sd_status, v, SD_STATUS_WMASK);
        break;
    default:
        break;
    }
}

static uint64_t sdhi_read(void *opaque, hwaddr addr, unsigned size)
{
    RenesasSDHIState *s = RENESAS_SDHI(opaque);
    hwaddr reg = addr & ~7;
    unsigned shift = (addr & 7) * 8;
    uint64_t v;

    if (addr >= SCC_BASE && addr < SCC_END) {
        qemu_log_mask(LOG_UNIMP, "%s: SCC read at 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
    /* In reset the registers hold their reset values (sdhi_ixrst) */
    if (reg == A_SD_BUF0 && !s->in_reset) {
        v = sdhi_buf_read(s, size) << shift;
        sdhi_update_irq(s);
    } else if (!sdhi_reg_known(s, reg)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: undefined offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        v = 0;
    } else {
        v = sdhi_reg_read(s, reg);
    }
    v = extract64(v, shift, size * 8);
    trace_renesas_sdhi_read(addr, v, size);
    return v;
}

static void sdhi_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    RenesasSDHIState *s = RENESAS_SDHI(opaque);
    hwaddr reg = addr & ~7;
    unsigned shift = (addr & 7) * 8;
    uint64_t mask = MAKE_64BIT_MASK(shift, size * 8);
    uint64_t old;

    trace_renesas_sdhi_write(addr, value, size);
    if (addr >= SCC_BASE && addr < SCC_END) {
        qemu_log_mask(LOG_UNIMP, "%s: SCC write at 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }
    if (s->in_reset) {
        return;
    }
    if (reg == A_SD_BUF0) {
        sdhi_buf_write(s, value, size);
    } else if (!sdhi_reg_known(s, reg)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: undefined offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return;
    } else {
        /* A narrower write leaves the other bytes as they read */
        old = reg == A_SD_INFO1 ? s->sd_info1 :
              reg == A_SD_INFO2 ? s->sd_info2 : sdhi_reg_read(s, reg);
        sdhi_reg_write(s, reg, (old & ~mask) | ((value << shift) & mask));
    }
    sdhi_update_irq(s);
}

static const MemoryRegionOps sdhi_ops = {
    .read = sdhi_read,
    .write = sdhi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 2,
    .impl.max_access_size = 8,
    .valid.min_access_size = 2,
    .valid.max_access_size = 8,
};

static void sdhi_set_inserted(DeviceState *dev, bool inserted)
{
    RenesasSDHIState *s = RENESAS_SDHI(dev);

    /* ISDCD falls on insertion (INFO4) and rises on removal (INFO3) */
    s->sd_info1 |= inserted ? R_SD_INFO1_INFO4_MASK : R_SD_INFO1_INFO3_MASK;
    sdhi_update_irq(s);
}

/* IXRST from the clock controller: @level 1 holds the channel in reset */
static void sdhi_ixrst(void *opaque, int n, int level)
{
    RenesasSDHIState *s = RENESAS_SDHI(opaque);

    trace_renesas_sdhi_ixrst(level);
    s->in_reset = level;
    if (level) {
        sdhi_reset_state(s);
        sdhi_update_irq(s);
    }
}

static void sdhi_reset_hold(Object *obj, ResetType type)
{
    RenesasSDHIState *s = RENESAS_SDHI(obj);

    sdhi_reset_state(s);
}

static void sdhi_reset_exit(Object *obj, ResetType type)
{
    sdhi_update_irq(RENESAS_SDHI(obj));
}

static void sdhi_init(Object *obj)
{
    RenesasSDHIState *s = RENESAS_SDHI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_RENESAS_SDHI_BUS,
              DEVICE(s), "sd-bus");
    memory_region_init_io(&s->iomem, obj, &sdhi_ops, s, TYPE_RENESAS_SDHI,
                          RENESAS_SDHI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->irq_sdio);
    qdev_init_gpio_in_named(DEVICE(obj), sdhi_ixrst, "ixrst", 1);
}

static void sdhi_realize(DeviceState *dev, Error **errp)
{
    RenesasSDHIState *s = RENESAS_SDHI(dev);

    if (!s->dma_mr) {
        error_setg(errp, TYPE_RENESAS_SDHI " 'dma-memory' link not set");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "renesas-sdhi-dma");
}

static const VMStateDescription vmstate_sdhi = {
    .name = TYPE_RENESAS_SDHI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(in_reset, RenesasSDHIState),
        VMSTATE_UINT32(sd_cmd, RenesasSDHIState),
        VMSTATE_UINT32(sd_arg, RenesasSDHIState),
        VMSTATE_UINT32(sd_stop, RenesasSDHIState),
        VMSTATE_UINT32(sd_seccnt, RenesasSDHIState),
        VMSTATE_UINT64(sd_rsp10, RenesasSDHIState),
        VMSTATE_UINT64(sd_rsp54, RenesasSDHIState),
        VMSTATE_UINT32(sd_info1, RenesasSDHIState),
        VMSTATE_UINT32(sd_info2, RenesasSDHIState),
        VMSTATE_UINT32(sd_info1_mask, RenesasSDHIState),
        VMSTATE_UINT32(sd_info2_mask, RenesasSDHIState),
        VMSTATE_UINT32(sd_clk_ctrl, RenesasSDHIState),
        VMSTATE_UINT32(sd_size, RenesasSDHIState),
        VMSTATE_UINT32(sd_option, RenesasSDHIState),
        VMSTATE_UINT32(sd_err_sts1, RenesasSDHIState),
        VMSTATE_UINT32(sd_err_sts2, RenesasSDHIState),
        VMSTATE_UINT32(sdio_mode, RenesasSDHIState),
        VMSTATE_UINT32(sdio_info1, RenesasSDHIState),
        VMSTATE_UINT32(sdio_info1_mask, RenesasSDHIState),
        VMSTATE_UINT32(cc_ext_mode, RenesasSDHIState),
        VMSTATE_UINT32(soft_rst, RenesasSDHIState),
        VMSTATE_UINT32(host_mode, RenesasSDHIState),
        VMSTATE_UINT32(sdif_mode, RenesasSDHIState),
        VMSTATE_UINT32(sd_status, RenesasSDHIState),
        VMSTATE_UINT32(dm_dtran_mode, RenesasSDHIState),
        VMSTATE_UINT32(dm_dtran_ctrl, RenesasSDHIState),
        VMSTATE_UINT32(dm_rst, RenesasSDHIState),
        VMSTATE_UINT32(dm_info1, RenesasSDHIState),
        VMSTATE_UINT32(dm_info1_mask, RenesasSDHIState),
        VMSTATE_UINT32(dm_info2, RenesasSDHIState),
        VMSTATE_UINT32(dm_info2_mask, RenesasSDHIState),
        VMSTATE_UINT32(dm_dtran_addr, RenesasSDHIState),
        VMSTATE_BOOL(busy, RenesasSDHIState),
        VMSTATE_BOOL(data_read, RenesasSDHIState),
        VMSTATE_BOOL(data_write, RenesasSDHIState),
        VMSTATE_BOOL(auto_cmd12, RenesasSDHIState),
        VMSTATE_BOOL(multi, RenesasSDHIState),
        VMSTATE_BOOL(dma_wait, RenesasSDHIState),
        VMSTATE_UINT32(blocks_left, RenesasSDHIState),
        VMSTATE_UINT32(buf_len, RenesasSDHIState),
        VMSTATE_UINT32(buf_pos, RenesasSDHIState),
        VMSTATE_UINT8_ARRAY(buf, RenesasSDHIState, RENESAS_SDHI_BLOCK_MAX),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sdhi_props[] = {
    DEFINE_PROP_BOOL("has-sd-status", RenesasSDHIState, has_sd_status, false),
    DEFINE_PROP_LINK("dma-memory", RenesasSDHIState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void sdhi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Renesas RZ/G2L SD/MMC host interface";
    dc->realize = sdhi_realize;
    dc->vmsd = &vmstate_sdhi;
    device_class_set_props(dc, sdhi_props);
    rc->phases.hold = sdhi_reset_hold;
    rc->phases.exit = sdhi_reset_exit;
}

static void sdhi_bus_class_init(ObjectClass *klass, const void *data)
{
    SDBusClass *sbc = SD_BUS_CLASS(klass);

    sbc->set_inserted = sdhi_set_inserted;
}

static const TypeInfo sdhi_types[] = {
    {
        .name = TYPE_RENESAS_SDHI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RenesasSDHIState),
        .instance_init = sdhi_init,
        .class_init = sdhi_class_init,
    },
    {
        .name = TYPE_RENESAS_SDHI_BUS,
        .parent = TYPE_SD_BUS,
        .instance_size = sizeof(SDBus),
        .class_init = sdhi_bus_class_init,
    },
};

DEFINE_TYPES(sdhi_types)
