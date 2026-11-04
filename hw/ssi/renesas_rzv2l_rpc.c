/*
 * Renesas RZ/V2L SPI Multi I/O Bus Controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The SPI Multi I/O Bus Controller of the RZ/V2L Group User's Manual:
 * Hardware, section 25, with one serial flash memory on QSPI0
 * (CMNCR.BSZ = 00): the 27 registers of Table 25.2, external address
 * space read mode and manual mode. The flash is a device on the "spi"
 * bus; one SSI transfer is one byte of the serial protocol.
 *
 * - Manual mode (section 25.5.7): writing SMCR.SPIE = 1 sends the phases
 *   SMENR enables, in the order of Table 25.4: command, optional command,
 *   address (ADE 0100, 0110, 0111, 1111: ADR[23:16], [23:8], [23:0],
 *   [31:0]), option data (OPD3 first), dummy cycles, then 1, 2 or 4 data
 *   bytes (SPIDE 1000, 1100, 1111), sent from SMWDR0 when SPIWE = 1 and
 *   stored in SMRDR0 when SPIRE = 1. SMCR.SSLKP = 1 keeps QSPI0_SSL
 *   asserted into the next transfer.
 * - Data registers: received data fills SMRDR0 from bit 0, byte 0 first
 *   (Figure 25.32), whatever the access size. A 4-byte transfer sends
 *   SMWDR0 as a little-endian word, byte 0 first; a 1- or 2-byte
 *   transfer sends the top bytes of the 32-bit register, lowest first,
 *   and a 1- or 2-byte write at offset 0 reaches those bytes. The manual
 *   has the guest write SMWDR0 at the transfer size (section 25.4.15);
 *   firmware written for this SoC writes 1 and 2 bytes as a 32-bit word
 *   with the data in the top bytes. The rule for writes serves both; the
 *   placement for a 32-bit write is an inference.
 * - External address space read mode (section 25.5.5): a read of the
 *   bus space sends the phases DRENR enables. With DRCR.RBE = 0 the
 *   access reads its own size. With RBE = 1 it goes through the read
 *   cache (section 25.5.6): a miss reads RBURST + 1 lines of 8 bytes
 *   from the 8-byte boundary into the lines from the write pointer on;
 *   a hit reads no flash. DRCR.RCF invalidates every line. With
 *   DRCR.SSLE = 1 QSPI0_SSL stays asserted after a burst, and the next
 *   miss at the following address continues it without command or
 *   address. A 32-bit address (ADE 1111) takes its upper bits from
 *   DREAR.EAV over the bits DREAR.EAC leaves to the bus space
 *   (section 25.5.3).
 * - CMNSR (section 25.5.12): every transfer completes when it starts.
 *   TEND reads 0 only while a burst read with SSLE = 1 holds QSPI0_SSL;
 *   SSLF reads the state of QSPI0_SSL.
 * - Dummy cycles are sent as bytes: cycles times the width of the
 *   address phase (the command phase when there is no address), over 8.
 * - Clock and reset: while the clock controller holds the reset, the
 *   controller is in its reset state; while it holds the reset or stops
 *   the clocks, every access to the registers or the bus space is
 *   refused with a bus error (the manual does not say what such an
 *   access does). The registers keep their values while the clocks are
 *   stopped.
 *
 * The bit sizes (1 or 4 lines) and SSLDR change only the wire timing,
 * which is not modelled; nor are the PHY timing adjustment (PHYCNT.CAL,
 * CKSEL, PHYOFFSET1/2, PHYADJ1/2). DDR transfers (PHYMEM = 01, the DDR
 * enable bits) move the same bytes as SDR and are logged once. Not
 * supported, each logged under unimp and refused with a bus error:
 * HyperFlash and Octal-SPI (PHYMEM = 11, HYPE, OCT), two flash memories
 * (BSZ = 01), the write buffer (WBUF, WBUF2), high-speed mode (HS). Not
 * modelled: the QSPI_RESET# and QSPI_WP# outputs (PHYINT), the SPIHF
 * interrupt, and the time a transfer takes.
 */

#include "qemu/osdep.h"
#include "hw/ssi/renesas_rzv2l_rpc.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

REG32(CMNCR, 0x00)
    FIELD(CMNCR, MD, 31, 1)
    FIELD(CMNCR, BSZ, 0, 2)
REG32(SSLDR, 0x04)
REG32(DRCR, 0x0c)
    FIELD(DRCR, SSLN, 24, 1)
    FIELD(DRCR, RBURST, 16, 5)
    FIELD(DRCR, RCF, 9, 1)
    FIELD(DRCR, RBE, 8, 1)
    FIELD(DRCR, SSLE, 0, 1)
REG32(DRCMR, 0x10)
REG32(DREAR, 0x14)
    FIELD(DREAR, EAV, 16, 8)
    FIELD(DREAR, EAC, 0, 3)
REG32(DROPR, 0x18)
REG32(DRENR, 0x1c)
REG32(SMCR, 0x20)
    FIELD(SMCR, SSLKP, 8, 1)
    FIELD(SMCR, SPIRE, 2, 1)
    FIELD(SMCR, SPIWE, 1, 1)
    FIELD(SMCR, SPIE, 0, 1)
REG32(SMCMR, 0x24)
REG32(SMADR, 0x28)
REG32(SMOPR, 0x2c)
REG32(SMENR, 0x30)
REG32(SMRDR0, 0x38)
REG32(SMRDR1, 0x3c)
REG32(SMWDR0, 0x40)
REG32(SMWDR1, 0x44)
REG32(CMNSR, 0x48)
    FIELD(CMNSR, SSLF, 1, 1)
    FIELD(CMNSR, TEND, 0, 1)
REG32(DRDMCR, 0x58)
REG32(DRDRENR, 0x5c)
REG32(SMDMCR, 0x60)
REG32(SMDRENR, 0x64)
REG32(PHYADJ1, 0x70)
REG32(PHYADJ2, 0x74)
REG32(PHYCNT, 0x7c)
    FIELD(PHYCNT, CAL, 31, 1)
    FIELD(PHYCNT, OCTA, 22, 2)
    FIELD(PHYCNT, OCT, 20, 1)
    FIELD(PHYCNT, HS, 18, 1)
    FIELD(PHYCNT, WBUF2, 4, 1)
    FIELD(PHYCNT, WBUF, 2, 1)
    FIELD(PHYCNT, PHYMEM, 0, 2)
REG32(PHYOFFSET1, 0x80)
REG32(PHYOFFSET2, 0x84)
REG32(PHYINT, 0x88)

/*
 * DRENR and SMENR share their layout (sections 25.4.7, 25.4.12); SPIDE
 * exists in SMENR only
 */
FIELD(EN, CDB, 30, 2)
FIELD(EN, OCDB, 28, 2)
FIELD(EN, ADB, 24, 2)
FIELD(EN, OPDB, 20, 2)
FIELD(EN, DB, 16, 2)
FIELD(EN, DME, 15, 1)
FIELD(EN, CDE, 14, 1)
FIELD(EN, OCDE, 12, 1)
FIELD(EN, ADE, 8, 4)
FIELD(EN, OPDE, 4, 4)
FIELD(EN, SPIDE, 0, 4)

/* DRCMR and SMCMR (sections 25.4.4, 25.4.9) */
FIELD(CMD, CMD, 16, 8)
FIELD(CMD, OCMD, 0, 8)

/* DRDRENR and SMDRENR (sections 25.4.19, 25.4.21) */
FIELD(DDR, HYPE, 12, 3)
FIELD(DDR, ADDRE, 8, 1)
FIELD(DDR, OPDRE, 4, 1)
FIELD(DDR, DRE, 0, 1)

/* DRDMCR and SMDMCR: n + 1 cycles, n from 1 to 19 */
FIELD(DMCR, DMCYC, 0, 5)

/* Reset values (Table 25.2) and writable bits (section 25.4) */
#define CMNCR_RESET         0x01557301
#define CMNCR_WMASK         0x80fff303
#define SSLDR_WMASK         0x00070707
#define DRCR_RESET          0x001f0100
#define DRCR_WMASK          0x001f0101
#define DRCMR_RESET         0x00a00000
#define CMD_WMASK           0x00ff00ff
#define DREAR_WMASK         0x00ff0007
#define DRENR_RESET         0xa222d400
#define DRENR_WMASK         0xf333dff0
#define SMCR_WMASK          0x00000106
#define SMENR_RESET         0x00004000
#define SMENR_WMASK         0xf333dfff
#define CMNSR_RESET         0x00000001
#define DRDMCR_RESET        0x0000000b
#define DMCR_WMASK          0x0000001f
#define DRDRENR_RESET       0x00005101
#define DDR_WMASK           0x00007111
#define PHYCNT_RESET        0x00000263
#define PHYCNT_WMASK        0x40f70017
#define PHYOFFSET1_RESET    0x21511144
#define PHYOFFSET1_WMASK    0x30000000
#define PHYOFFSET2_RESET    0x00000431
#define PHYOFFSET2_WMASK    0x00000700
#define PHYINT_RESET        0x07070002
#define PHYINT_WMASK        0x07000006

/* One command, optional command, address, option data and dummy phase */
#define RPC_HEADER_MAX      (1 + 1 + 4 + 4 + 10)

typedef struct RPCHeader {
    uint8_t buf[RPC_HEADER_MAX];
    int len;
} RPCHeader;

static void rpc_set_ssl(RenesasRZV2LRPCState *s, bool asserted)
{
    if (s->ssl != asserted) {
        s->ssl = asserted;
        trace_renesas_rzv2l_rpc_ssl(asserted);
        qemu_set_irq(s->cs, !asserted);
    }
    if (!asserted) {
        s->burst_held = false;
    }
}

static void rpc_cache_flush(RenesasRZV2LRPCState *s)
{
    memset(s->cache_valid, 0, sizeof(s->cache_valid));
}

static void rpc_reset_state(RenesasRZV2LRPCState *s)
{
    rpc_set_ssl(s, false);
    s->cmncr = CMNCR_RESET;
    s->ssldr = 0;
    s->drcr = DRCR_RESET;
    s->drcmr = DRCMR_RESET;
    s->drear = 0;
    s->dropr = 0;
    s->drenr = DRENR_RESET;
    s->smcr = 0;
    s->smcmr = 0;
    s->smadr = 0;
    s->smopr = 0;
    s->smenr = SMENR_RESET;
    s->smrdr0 = 0;
    s->smrdr1 = 0;
    s->smwdr0 = 0;
    s->smwdr1 = 0;
    s->drdmcr = DRDMCR_RESET;
    s->drdrenr = DRDRENR_RESET;
    s->smdmcr = 0;
    s->smdrenr = 0;
    s->phyadj1 = 0;
    s->phyadj2 = 0;
    s->phycnt = PHYCNT_RESET;
    s->phyoffset1 = PHYOFFSET1_RESET;
    s->phyoffset2 = PHYOFFSET2_RESET;
    s->phyint = PHYINT_RESET;
    s->next_addr = 0;
    s->ddr_logged = false;
    s->cache_wp = 0;
    rpc_cache_flush(s);
    memset(s->cache_tag, 0, sizeof(s->cache_tag));
    memset(s->cache_data, 0, sizeof(s->cache_data));
}

/* The clock controller lets the controller be accessed */
static bool rpc_accessible(RenesasRZV2LRPCState *s, const char *what)
{
    if (s->in_reset || !s->clk_on) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-rzv2l-rpc: %s while the controller is %s\n",
                      what, s->in_reset ? "held in reset" : "not clocked");
        return false;
    }
    return true;
}

/*
 * The connected memory and transfer mode are ones the model supports:
 * one serial flash memory, no write buffer, no high-speed mode
 */
static bool rpc_config_ok(RenesasRZV2LRPCState *s, uint32_t ddr)
{
    unsigned phymem = FIELD_EX32(s->phycnt, PHYCNT, PHYMEM);
    const char *why = NULL;

    if (phymem == 3) {
        why = "HyperFlash (PHYCNT.PHYMEM = 11)";
    } else if (phymem == 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-rzv2l-rpc: PHYCNT.PHYMEM = 10 is prohibited\n");
        return false;
    } else if (FIELD_EX32(s->phycnt, PHYCNT, OCT) ||
               FIELD_EX32(s->phycnt, PHYCNT, OCTA)) {
        why = "Octal-SPI flash memory (PHYCNT.OCT, OCTA)";
    } else if (FIELD_EX32(ddr, DDR, HYPE)) {
        why = "HyperFlash or Octal-SPI DDR mode (HYPE)";
    } else if (FIELD_EX32(s->cmncr, CMNCR, BSZ) != 0) {
        why = "two flash memories (CMNCR.BSZ = 01)";
    } else if (FIELD_EX32(s->phycnt, PHYCNT, WBUF) ||
               FIELD_EX32(s->phycnt, PHYCNT, WBUF2)) {
        why = "the write buffer (PHYCNT.WBUF, WBUF2)";
    } else if (FIELD_EX32(s->phycnt, PHYCNT, HS)) {
        why = "high-speed mode (PHYCNT.HS)";
    }
    if (why) {
        qemu_log_mask(LOG_UNIMP, "renesas-rzv2l-rpc: transfer with %s\n",
                      why);
        return false;
    }
    if (!s->ddr_logged && (phymem == 1 ||
        (ddr & (R_DDR_ADDRE_MASK | R_DDR_OPDRE_MASK | R_DDR_DRE_MASK)))) {
        qemu_log_mask(LOG_UNIMP,
                      "renesas-rzv2l-rpc: DDR transfer modelled as SDR\n");
        s->ddr_logged = true;
    }
    return true;
}

/* Lines of a 1-bit (00) or 4-bit (10) phase; 0 for a prohibited value */
static unsigned rpc_lanes(unsigned bits)
{
    switch (bits) {
    case 0:
        return 1;
    case 2:
        return 4;
    default:
        return 0;
    }
}

/*
 * Build the command, optional command, address, option data and dummy
 * phases @en enables. Returns false, logged, for a prohibited setting.
 */
static bool rpc_build_header(RPCHeader *h, uint32_t en, uint32_t cmd,
                             uint32_t addr, uint32_t opd, uint32_t dmcr,
                             bool manual)
{
    unsigned ade = FIELD_EX32(en, EN, ADE);
    unsigned opde = FIELD_EX32(en, EN, OPDE);
    unsigned top, naddr, nopd, lanes;
    int i;

    h->len = 0;
    if (!rpc_lanes(FIELD_EX32(en, EN, CDB)) ||
        !rpc_lanes(FIELD_EX32(en, EN, OCDB)) ||
        !rpc_lanes(FIELD_EX32(en, EN, ADB)) ||
        !rpc_lanes(FIELD_EX32(en, EN, OPDB)) ||
        !rpc_lanes(FIELD_EX32(en, EN, DB))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-rzv2l-rpc: prohibited bit size in 0x%08x\n",
                      en);
        return false;
    }
    switch (ade) {
    case 0x0:
        naddr = 0;
        top = 0;
        break;
    case 0x4:
    case 0x6:
        /* ADR[23:16], ADR[23:8]: manual mode only */
        naddr = manual ? (ade == 0x4 ? 1 : 2) : 0;
        top = 2;
        break;
    case 0x7:
        naddr = 3;
        top = 2;
        break;
    case 0xf:
        naddr = 4;
        top = 3;
        break;
    default:
        naddr = 0;
        top = 0;
        break;
    }
    if (ade && !naddr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-rzv2l-rpc: prohibited ADE 0x%x\n", ade);
        return false;
    }
    switch (opde) {
    case 0x0:
    case 0x8:
    case 0xc:
    case 0xe:
    case 0xf:
        nopd = ctpop32(opde);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-rzv2l-rpc: prohibited OPDE 0x%x\n", opde);
        return false;
    }

    if (FIELD_EX32(en, EN, CDE)) {
        h->buf[h->len++] = FIELD_EX32(cmd, CMD, CMD);
    }
    if (FIELD_EX32(en, EN, OCDE)) {
        h->buf[h->len++] = FIELD_EX32(cmd, CMD, OCMD);
    }
    for (i = 0; i < naddr; i++) {
        h->buf[h->len++] = addr >> (8 * (top - i));
    }
    for (i = 0; i < nopd; i++) {
        h->buf[h->len++] = opd >> (8 * (3 - i));
    }
    if (FIELD_EX32(en, EN, DME)) {
        unsigned dmcyc = FIELD_EX32(dmcr, DMCR, DMCYC);
        unsigned bits;

        if (dmcyc < 1 || dmcyc > 19) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "renesas-rzv2l-rpc: prohibited DMCYC %u\n", dmcyc);
            return false;
        }
        lanes = rpc_lanes(naddr ? FIELD_EX32(en, EN, ADB)
                                : FIELD_EX32(en, EN, CDB));
        bits = (dmcyc + 1) * lanes;
        if (bits % 8) {
            qemu_log_mask(LOG_UNIMP,
                          "renesas-rzv2l-rpc: %u dummy cycles on %u lines "
                          "are not a whole byte\n", dmcyc + 1, lanes);
            return false;
        }
        for (i = 0; i < bits / 8; i++) {
            h->buf[h->len++] = 0xff;
        }
    }
    return true;
}

static void rpc_send_header(RenesasRZV2LRPCState *s, const RPCHeader *h)
{
    int i;

    for (i = 0; i < h->len; i++) {
        ssi_transfer(s->spi, h->buf[i]);
    }
}

/* SMENR.SPIDE: the number of data bytes of a manual transfer, or -1 */
static int rpc_spide_bytes(uint32_t smenr)
{
    switch (FIELD_EX32(smenr, EN, SPIDE)) {
    case 0x0:
        return 0;
    case 0x8:
        return 1;
    case 0xc:
        return 2;
    case 0xf:
        return 4;
    default:
        return -1;
    }
}

/*
 * A manual mode transfer (SMCR.SPIE = 1). Returns false when the
 * transfer needs a mode the model does not support.
 */
static bool rpc_manual_xfer(RenesasRZV2LRPCState *s)
{
    bool spire = FIELD_EX32(s->smcr, SMCR, SPIRE);
    bool spiwe = FIELD_EX32(s->smcr, SMCR, SPIWE);
    bool sslkp = FIELD_EX32(s->smcr, SMCR, SSLKP);
    int n = rpc_spide_bytes(s->smenr);
    uint32_t rdata = 0;
    RPCHeader h;
    int i;

    if (!FIELD_EX32(s->cmncr, CMNCR, MD)) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas-rzv2l-rpc: SMCR.SPIE in "
                      "external address space read mode\n");
        return true;
    }
    if (!rpc_config_ok(s, s->smdrenr)) {
        return false;
    }
    if (n < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-rzv2l-rpc: prohibited SPIDE 0x%x\n",
                      FIELD_EX32(s->smenr, EN, SPIDE));
        return true;
    }
    if (!rpc_build_header(&h, s->smenr, s->smcmr, s->smadr, s->smopr,
                          s->smdmcr, true)) {
        return true;
    }
    if (!h.len && !n) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-rzv2l-rpc: manual transfer of nothing\n");
        return true;
    }
    if (spire && sslkp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-rzv2l-rpc: read with SMCR.SSLKP = 1\n");
    }
    if (s->burst_held) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas-rzv2l-rpc: manual transfer "
                      "while a burst read holds QSPI0_SSL\n");
        s->burst_held = false;
    }

    trace_renesas_rzv2l_rpc_manual(FIELD_EX32(s->smcmr, CMD, CMD), s->smenr,
                                   s->smadr, n, spiwe, spire);
    rpc_set_ssl(s, true);
    rpc_send_header(s, &h);
    for (i = 0; i < n; i++) {
        uint8_t tx = spiwe ? s->smwdr0 >> (8 * (4 - n + i)) : 0;
        uint8_t rx = ssi_transfer(s->spi, tx);

        rdata |= (uint32_t)rx << (8 * i);
    }
    if (spire && n) {
        s->smrdr0 = rdata;
    }
    if (!sslkp) {
        rpc_set_ssl(s, false);
    }
    return true;
}

/*
 * The serial flash address of bus space offset @offset (section 25.5.3);
 * with no address phase the bus space offset tags the read cache
 */
static bool rpc_flash_addr(RenesasRZV2LRPCState *s, hwaddr offset,
                           uint32_t *addr)
{
    switch (FIELD_EX32(s->drenr, EN, ADE)) {
    case 0xf: {
        unsigned eac = FIELD_EX32(s->drear, DREAR, EAC);
        uint64_t mask;

        if (eac > 3) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "renesas-rzv2l-rpc: prohibited DREAR.EAC %u\n", eac);
            return false;
        }
        mask = MAKE_64BIT_MASK(0, 25 + eac);
        *addr = (((uint64_t)FIELD_EX32(s->drear, DREAR, EAV) << 25) & ~mask) |
                (offset & mask);
        return true;
    }
    case 0x7:
        *addr = offset & MAKE_64BIT_MASK(0, 24);
        return true;
    default:
        *addr = offset;
        return true;
    }
}

/* One external address space read without the cache (RBE = 0) */
static bool rpc_normal_read(RenesasRZV2LRPCState *s, uint32_t addr,
                            unsigned size, uint64_t *data)
{
    RPCHeader h;
    unsigned i;

    if (!rpc_build_header(&h, s->drenr, s->drcmr, addr, s->dropr,
                          s->drdmcr, false)) {
        return false;
    }
    trace_renesas_rzv2l_rpc_read(FIELD_EX32(s->drcmr, CMD, CMD), addr, size,
                                 false);
    rpc_set_ssl(s, false);
    rpc_set_ssl(s, true);
    rpc_send_header(s, &h);
    *data = 0;
    for (i = 0; i < size; i++) {
        *data |= (uint64_t)ssi_transfer(s->spi, 0) << (8 * i);
    }
    rpc_set_ssl(s, false);
    return true;
}

/* A read cache miss at line @tag: burst read RBURST + 1 lines */
static bool rpc_burst_fill(RenesasRZV2LRPCState *s, uint32_t tag)
{
    unsigned lines = FIELD_EX32(s->drcr, DRCR, RBURST) + 1;
    uint32_t addr = tag * RENESAS_RZV2L_RPC_LINE_SIZE;
    bool cont = s->burst_held && addr == s->next_addr;
    unsigned i, j;

    if (!cont) {
        RPCHeader h;

        if (!rpc_build_header(&h, s->drenr, s->drcmr, addr, s->dropr,
                              s->drdmcr, false)) {
            return false;
        }
        rpc_set_ssl(s, false);
        rpc_set_ssl(s, true);
        rpc_send_header(s, &h);
    }
    trace_renesas_rzv2l_rpc_read(FIELD_EX32(s->drcmr, CMD, CMD), addr,
                                 lines * RENESAS_RZV2L_RPC_LINE_SIZE, true);
    for (i = 0; i < lines; i++) {
        unsigned e = s->cache_wp;

        for (j = 0; j < RENESAS_RZV2L_RPC_LINE_SIZE; j++) {
            s->cache_data[e][j] = ssi_transfer(s->spi, 0);
        }
        s->cache_tag[e] = tag + i;
        s->cache_valid[e] = true;
        s->cache_wp = (e + 1) % RENESAS_RZV2L_RPC_CACHE_LINES;
    }
    if (FIELD_EX32(s->drcr, DRCR, SSLE)) {
        s->burst_held = true;
        s->next_addr = addr + lines * RENESAS_RZV2L_RPC_LINE_SIZE;
    } else {
        rpc_set_ssl(s, false);
    }
    return true;
}

static int rpc_cache_lookup(RenesasRZV2LRPCState *s, uint32_t tag)
{
    int e;

    for (e = 0; e < RENESAS_RZV2L_RPC_CACHE_LINES; e++) {
        if (s->cache_valid[e] && s->cache_tag[e] == tag) {
            return e;
        }
    }
    return -1;
}

/* One external address space read through the read cache (RBE = 1) */
static bool rpc_cached_read(RenesasRZV2LRPCState *s, uint32_t addr,
                            unsigned size, uint64_t *data)
{
    unsigned i;

    *data = 0;
    for (i = 0; i < size; i++) {
        uint32_t a = addr + i;
        uint32_t tag = a / RENESAS_RZV2L_RPC_LINE_SIZE;
        int e = rpc_cache_lookup(s, tag);

        if (e < 0) {
            if (!rpc_burst_fill(s, tag)) {
                return false;
            }
            e = rpc_cache_lookup(s, tag);
            assert(e >= 0);
        }
        *data |= (uint64_t)s->cache_data[e][a % RENESAS_RZV2L_RPC_LINE_SIZE]
                 << (8 * i);
    }
    return true;
}

static MemTxResult rpc_window_read(void *opaque, hwaddr offset,
                                   uint64_t *data, unsigned size,
                                   MemTxAttrs attrs)
{
    RenesasRZV2LRPCState *s = RENESAS_RZV2L_RPC(opaque);
    uint32_t addr;
    bool ok;

    *data = 0;
    if (!rpc_accessible(s, "read of the SPI multi I/O bus space")) {
        return MEMTX_ERROR;
    }
    if (FIELD_EX32(s->cmncr, CMNCR, MD)) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas-rzv2l-rpc: read of the SPI "
                      "multi I/O bus space in manual mode\n");
        return MEMTX_ERROR;
    }
    if (!rpc_config_ok(s, s->drdrenr) ||
        !rpc_flash_addr(s, offset, &addr)) {
        return MEMTX_ERROR;
    }
    if (FIELD_EX32(s->drcr, DRCR, RBE)) {
        ok = rpc_cached_read(s, addr, size, data);
    } else {
        ok = rpc_normal_read(s, addr, size, data);
    }
    return ok ? MEMTX_OK : MEMTX_ERROR;
}

static MemTxResult rpc_window_write(void *opaque, hwaddr offset,
                                    uint64_t data, unsigned size,
                                    MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR, "renesas-rzv2l-rpc: write to the SPI "
                  "multi I/O bus space at 0x%" HWADDR_PRIx "\n", offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps rpc_window_ops = {
    .read_with_attrs = rpc_window_read,
    .write_with_attrs = rpc_window_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static bool rpc_is_data_reg(hwaddr offset)
{
    switch (offset & ~3) {
    case A_SMRDR0:
    case A_SMRDR1:
    case A_SMWDR0:
    case A_SMWDR1:
        return true;
    default:
        return false;
    }
}

/* SMWDR0: a 1- or 2-byte access reaches the top bytes */
static uint32_t rpc_wdata_read(uint32_t reg, unsigned size)
{
    return size == 4 ? reg : reg >> (32 - 8 * size);
}

static uint32_t rpc_wdata_write(uint32_t val, unsigned size)
{
    return size == 4 ? val : val << (32 - 8 * size);
}

static MemTxResult rpc_read(void *opaque, hwaddr offset, uint64_t *data,
                            unsigned size, MemTxAttrs attrs)
{
    RenesasRZV2LRPCState *s = RENESAS_RZV2L_RPC(opaque);
    uint32_t v;

    *data = 0;
    if (!rpc_accessible(s, "register read")) {
        return MEMTX_ERROR;
    }
    if (rpc_is_data_reg(offset) ? (offset & 3) != 0 : size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas-rzv2l-rpc: %u-byte read at "
                      "0x%" HWADDR_PRIx "\n", size, offset);
        return MEMTX_ERROR;
    }

    switch (offset) {
    case A_CMNCR:
        v = s->cmncr;
        break;
    case A_SSLDR:
        v = s->ssldr;
        break;
    case A_DRCR:
        v = s->drcr;
        break;
    case A_DRCMR:
        v = s->drcmr;
        break;
    case A_DREAR:
        v = s->drear;
        break;
    case A_DROPR:
        v = s->dropr;
        break;
    case A_DRENR:
        v = s->drenr;
        break;
    case A_SMCR:
        v = s->smcr;
        break;
    case A_SMCMR:
        v = s->smcmr;
        break;
    case A_SMADR:
        v = s->smadr;
        break;
    case A_SMOPR:
        v = s->smopr;
        break;
    case A_SMENR:
        v = s->smenr;
        break;
    case A_SMRDR0:
        v = s->smrdr0;
        break;
    case A_SMRDR1:
        v = s->smrdr1;
        break;
    case A_SMWDR0:
        v = rpc_wdata_read(s->smwdr0, size);
        break;
    case A_SMWDR1:
        v = rpc_wdata_read(s->smwdr1, size);
        break;
    case A_CMNSR:
        v = FIELD_DP32(0, CMNSR, TEND, !s->burst_held);
        v = FIELD_DP32(v, CMNSR, SSLF, s->ssl);
        break;
    case A_DRDMCR:
        v = s->drdmcr;
        break;
    case A_DRDRENR:
        v = s->drdrenr;
        break;
    case A_SMDMCR:
        v = s->smdmcr;
        break;
    case A_SMDRENR:
        v = s->smdrenr;
        break;
    case A_PHYADJ1:
        v = s->phyadj1;
        break;
    case A_PHYADJ2:
        v = s->phyadj2;
        break;
    case A_PHYCNT:
        v = s->phycnt;
        break;
    case A_PHYOFFSET1:
        v = s->phyoffset1;
        break;
    case A_PHYOFFSET2:
        v = s->phyoffset2;
        break;
    case A_PHYINT:
        v = s->phyint;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "renesas-rzv2l-rpc: read of the "
                      "undefined offset 0x%" HWADDR_PRIx "\n", offset);
        v = 0;
        break;
    }
    trace_renesas_rzv2l_rpc_reg_read(offset, v, size);
    *data = v;
    return MEMTX_OK;
}

static inline void rpc_wr(uint32_t *reg, uint32_t val, uint32_t wmask)
{
    *reg = (*reg & ~wmask) | (val & wmask);
}

static MemTxResult rpc_write(void *opaque, hwaddr offset, uint64_t data,
                             unsigned size, MemTxAttrs attrs)
{
    RenesasRZV2LRPCState *s = RENESAS_RZV2L_RPC(opaque);
    uint32_t val = data;

    if (!rpc_accessible(s, "register write")) {
        return MEMTX_ERROR;
    }
    if (rpc_is_data_reg(offset) ? (offset & 3) != 0 : size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas-rzv2l-rpc: %u-byte write at "
                      "0x%" HWADDR_PRIx "\n", size, offset);
        return MEMTX_ERROR;
    }
    trace_renesas_rzv2l_rpc_reg_write(offset, val, size);

    switch (offset) {
    case A_CMNCR:
        rpc_wr(&s->cmncr, val, CMNCR_WMASK);
        break;
    case A_SSLDR:
        rpc_wr(&s->ssldr, val, SSLDR_WMASK);
        break;
    case A_DRCR:
        rpc_wr(&s->drcr, val, DRCR_WMASK);
        if (FIELD_EX32(val, DRCR, SSLN) && FIELD_EX32(s->drcr, DRCR, RBE) &&
            FIELD_EX32(s->drcr, DRCR, SSLE)) {
            rpc_set_ssl(s, false);
        }
        if (FIELD_EX32(val, DRCR, RCF)) {
            rpc_cache_flush(s);
        }
        break;
    case A_DRCMR:
        rpc_wr(&s->drcmr, val, CMD_WMASK);
        break;
    case A_DREAR:
        rpc_wr(&s->drear, val, DREAR_WMASK);
        break;
    case A_DROPR:
        s->dropr = val;
        break;
    case A_DRENR:
        rpc_wr(&s->drenr, val, DRENR_WMASK);
        break;
    case A_SMCR:
        rpc_wr(&s->smcr, val, SMCR_WMASK);
        if (FIELD_EX32(val, SMCR, SPIE) && !rpc_manual_xfer(s)) {
            return MEMTX_DECODE_ERROR;
        }
        break;
    case A_SMCMR:
        rpc_wr(&s->smcmr, val, CMD_WMASK);
        break;
    case A_SMADR:
        s->smadr = val;
        break;
    case A_SMOPR:
        s->smopr = val;
        break;
    case A_SMENR:
        rpc_wr(&s->smenr, val, SMENR_WMASK);
        break;
    case A_SMWDR0:
        s->smwdr0 = rpc_wdata_write(val, size);
        break;
    case A_SMWDR1:
        s->smwdr1 = rpc_wdata_write(val, size);
        break;
    case A_DRDMCR:
        rpc_wr(&s->drdmcr, val, DMCR_WMASK);
        break;
    case A_DRDRENR:
        rpc_wr(&s->drdrenr, val, DDR_WMASK);
        break;
    case A_SMDMCR:
        rpc_wr(&s->smdmcr, val, DMCR_WMASK);
        break;
    case A_SMDRENR:
        rpc_wr(&s->smdrenr, val, DDR_WMASK);
        break;
    case A_PHYADJ1:
        s->phyadj1 = val;
        break;
    case A_PHYADJ2:
        s->phyadj2 = val;
        break;
    case A_PHYCNT:
        rpc_wr(&s->phycnt, val, PHYCNT_WMASK);
        break;
    case A_PHYOFFSET1:
        rpc_wr(&s->phyoffset1, val, PHYOFFSET1_WMASK);
        break;
    case A_PHYOFFSET2:
        rpc_wr(&s->phyoffset2, val, PHYOFFSET2_WMASK);
        break;
    case A_PHYINT:
        rpc_wr(&s->phyint, val, PHYINT_WMASK);
        break;
    case A_SMRDR0:
    case A_SMRDR1:
    case A_CMNSR:
        qemu_log_mask(LOG_GUEST_ERROR, "renesas-rzv2l-rpc: write to the "
                      "read-only register at 0x%" HWADDR_PRIx "\n", offset);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "renesas-rzv2l-rpc: write to the "
                      "undefined offset 0x%" HWADDR_PRIx "\n", offset);
        break;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps rpc_ops = {
    .read_with_attrs = rpc_read,
    .write_with_attrs = rpc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* The clock controller's reset line: high while the reset is applied */
static void rpc_rst(void *opaque, int n, int level)
{
    RenesasRZV2LRPCState *s = RENESAS_RZV2L_RPC(opaque);

    trace_renesas_rzv2l_rpc_rst(level);
    s->in_reset = level;
    if (level) {
        rpc_reset_state(s);
    }
}

/* The clock controller's clock line: high while the clocks run */
static void rpc_clk(void *opaque, int n, int level)
{
    RenesasRZV2LRPCState *s = RENESAS_RZV2L_RPC(opaque);

    trace_renesas_rzv2l_rpc_clk(level);
    s->clk_on = level;
}

static void rpc_reset_hold(Object *obj, ResetType type)
{
    rpc_reset_state(RENESAS_RZV2L_RPC(obj));
}

static void rpc_init(Object *obj)
{
    RenesasRZV2LRPCState *s = RENESAS_RZV2L_RPC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rpc_ops, s, "renesas-rzv2l-rpc",
                          RENESAS_RZV2L_RPC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    memory_region_init_io(&s->window, obj, &rpc_window_ops, s,
                          "renesas-rzv2l-rpc.dirmap",
                          RENESAS_RZV2L_RPC_WINDOW_SIZE);
    sysbus_init_mmio(sbd, &s->window);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->cs);
    qdev_init_gpio_in_named(DEVICE(obj), rpc_rst, "rst", 1);
    qdev_init_gpio_in_named(DEVICE(obj), rpc_clk, "clk", 1);
    s->spi = ssi_create_bus(DEVICE(obj), "spi");
}

static const VMStateDescription vmstate_rpc = {
    .name = TYPE_RENESAS_RZV2L_RPC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(in_reset, RenesasRZV2LRPCState),
        VMSTATE_BOOL(clk_on, RenesasRZV2LRPCState),
        VMSTATE_UINT32(cmncr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(ssldr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(drcr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(drcmr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(drear, RenesasRZV2LRPCState),
        VMSTATE_UINT32(dropr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(drenr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smcr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smcmr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smadr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smopr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smenr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smrdr0, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smrdr1, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smwdr0, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smwdr1, RenesasRZV2LRPCState),
        VMSTATE_UINT32(drdmcr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(drdrenr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smdmcr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(smdrenr, RenesasRZV2LRPCState),
        VMSTATE_UINT32(phyadj1, RenesasRZV2LRPCState),
        VMSTATE_UINT32(phyadj2, RenesasRZV2LRPCState),
        VMSTATE_UINT32(phycnt, RenesasRZV2LRPCState),
        VMSTATE_UINT32(phyoffset1, RenesasRZV2LRPCState),
        VMSTATE_UINT32(phyoffset2, RenesasRZV2LRPCState),
        VMSTATE_UINT32(phyint, RenesasRZV2LRPCState),
        VMSTATE_BOOL(ssl, RenesasRZV2LRPCState),
        VMSTATE_BOOL(burst_held, RenesasRZV2LRPCState),
        VMSTATE_UINT32(next_addr, RenesasRZV2LRPCState),
        VMSTATE_BOOL(ddr_logged, RenesasRZV2LRPCState),
        VMSTATE_UINT32(cache_wp, RenesasRZV2LRPCState),
        VMSTATE_BOOL_ARRAY(cache_valid, RenesasRZV2LRPCState,
                           RENESAS_RZV2L_RPC_CACHE_LINES),
        VMSTATE_UINT32_ARRAY(cache_tag, RenesasRZV2LRPCState,
                             RENESAS_RZV2L_RPC_CACHE_LINES),
        VMSTATE_UINT8_2DARRAY(cache_data, RenesasRZV2LRPCState,
                              RENESAS_RZV2L_RPC_CACHE_LINES,
                              RENESAS_RZV2L_RPC_LINE_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void rpc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Renesas RZ/V2L SPI Multi I/O Bus Controller";
    dc->vmsd = &vmstate_rpc;
    rc->phases.hold = rpc_reset_hold;
}

static const TypeInfo rpc_info = {
    .name = TYPE_RENESAS_RZV2L_RPC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZV2LRPCState),
    .instance_init = rpc_init,
    .class_init = rpc_class_init,
};

static void rpc_register_types(void)
{
    type_register_static(&rpc_info);
}

type_init(rpc_register_types)
