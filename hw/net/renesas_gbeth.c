/*
 * Renesas RZ/G2L family Gigabit Ethernet interface (GbEthernet)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * RZ/V2L Group User's Manual: Hardware (R01UH0936EJ), chapter 30.
 * Register offsets and reset values from Tables 30.4-30.6.
 *
 * DMAC (section 30.5.1-30.5.5): one transmission queue (q = 0) and one
 * reception queue (q = 4) of 8-byte descriptors, found through the
 * descriptor base address table at DBAT (entry q at DBAT + 8 * q).
 * - CCC.OPC selects RESET, CONFIG or OPERATION; CSR.OPS = 1 << OPC.
 *   Entering OPERATION loads the current descriptor addresses from the
 *   table; leaving it clears them and the status flags, and CONFIG mode
 *   returns all status registers to their reset values (section 30.4.1.5,
 *   30.5.1.1).
 * - Transmission runs on a TCCR.TSRQ0 write, at once, until the queue
 *   reaches a descriptor the software owns (FEMPTY, LEMPTY, EEMPTY), where
 *   the next request starts again, or an EOS, after which it starts at
 *   the next descriptor (Table 30.14); TSRQ0 then reads 0.
 *   FSTART/FMID/FEND/FSINGLE descriptors are written back as FEMPTY,
 *   LINK as LEMPTY, EOS as EEMPTY; LINKFIX is followed and not written
 *   back. A frame is sent when its last descriptor has been read;
 *   TIS.FTF0 is set.
 * - Reception stores each frame into FEMPTY descriptors, split over as
 *   many as DS requires (FSINGLE, or FSTART/FMID/FEND), with the frame
 *   length or the descriptor's part of it in DS and MSC (Tables 30.12,
 *   30.13). FEMPTY_ND descriptors receive DS = 0 and no data. RIS0.FRF0
 *   is set per stored frame. A frame that finds a descriptor the
 *   software owns is discarded, with RIS2.QFF0; the queue retries the
 *   same descriptor with the next frame. EI is set only when storing the
 *   data fails: that descriptor is written back and the queue moves on
 *   (section 30.5.5.4).
 * - Descriptor types the manual reserves, or a type the queue does not
 *   process (FEMPTY_ND on transmission), stop the queue at that
 *   descriptor: EIS.QEF, ESR.ET = 2 (section 30.5.5.3). A failed bus
 *   access sets EIS.QEF with ESR.ET for a descriptor read (0) or write
 *   (1), or a data read (4) or write (5). The frame is lost.
 * - Flags in DIS, EIS, RIS0-3 and TIS clear when 0 is written to them;
 *   a 1 has no effect. The enable registers have set (xIE) and clear
 *   (xID) aliases that read 0.
 * - pif_int_n is the OR of the ISS summary bits (section 30.4.1.43):
 *   each enabled DMAC flag, EIS.QFS, and the E-MAC flags in CXR21
 *   enabled in CXR22. CIE gates the descriptor-processed flags of
 *   RIS3 and TIS in pif_int_n, not in ISS (Table 30.20, section
 *   30.4.1.42). int_fil_n and int_arp_ns_n, the TOE
 *   filter and ARP/NS outputs, stay inactive.
 *
 * E-MAC (section 30.4.2):
 * - CXR20 (ECMR) TPE/RPE enable transmission and reception. A frame
 *   waits in the queue while TPE is 0. Received frames pass the address
 *   filter (CXR24/CXR25, broadcast, multicast, or CXR20.PRM), and frames
 *   longer than CXR2A allows are discarded. Short frames are padded to
 *   60 bytes, as on the wire.
 * - CXR21 (ECSR): LINKI is set when the link selected by CXR31 changes;
 *   FCI and LINKI clear when 1 is written to them. CXR2B (PSR) reports
 *   the selected link: the PHY's link through the RGMII in-band status
 *   and the LINKSTA pin, CXR2G, or CXR20.TPE.
 * - CXR23 (PIR) is the MDIO pin interface: a clause 22 frame clocked on
 *   MDC reaches the PHY at "phy-addr"; a read frame for another
 *   address, and a clause 45 frame, get no answer (MDI reads 1).
 * - The frame counters read 0, except CXR57 (multicast frames
 *   received; broadcast frames are not counted, nor marked in MSC bit 7,
 *   as the manual names them apart, an inference); they clear on a
 *   write, or on a read with CXR20.TRCCM.
 *
 * TOE (section 30.5.6): with CSR0.TPE, the IPv4 header checksum and the
 * TCP/UDP/ICMP checksums of IPv4 and IPv6 frames that CSR1 enables are
 * computed into transmitted frames (an IPv4 UDP checksum of 0 stays 0).
 * With CXR20.RCPT and CSR0.RPE, a 4-byte result follows each received
 * frame in place of the CRC (Figure 30.25): IPv4 header, then the
 * TCP/UDP/ICMP checksum, each 0 when correct, H'FFFF when the frame or
 * the protocol is not supported or not enabled in CSR2, the computed
 * value when wrong (MSC bit 6); an unsupported frame sets MSC bit 5.
 * With RCPT and without RPE, the CRC is stored.
 *
 * Not modelled: timing (transfers complete when they are requested, so
 * CSR.TPO/RPO read 0), the reception FIFO critical and overflow levels,
 * flow control and PAUSE frames, the DMAC data-path suspend and RX
 * descriptor fetch stop (CCC.DTSR/RDFD), DLR reloads, the hardware MDIO
 * controller (0x780-0x78C; MDIOMOD must keep SEL_MDIO_N = 1), EEE/LPI,
 * the TOE filter, wake-up and ARP/NS functions, IPv6 extension headers
 * and IPv4 options in the TOE (they read as unsupported), RCR.EFFS
 * (error frames are never received), and frames longer than 1.5 KB on
 * transmission (logged, dropped, descriptors written back). Accesses to
 * these registers, and to offsets the manual does not list, fail with a
 * decode error. While ETHn_RST_HW_N is low, registers read their reset
 * values and writes are ignored.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/net/renesas_gbeth.h"
#include "migration/vmstate.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "system/dma.h"
#include "trace.h"
#include <zlib.h>

/* DMAC (Table 30.4) */
#define A_CCC           0x000
#define   CCC_OPC_MASK      0x3
#define   CCC_RW            0x02040303  /* BOC: the CPU writes only 0 */
#define A_DBAT          0x004
#define A_DLR           0x008
#define   DLR_RESET         0x003fffff
#define A_CSR           0x00c
#define A_CDAR0         0x010
#define A_CDAR4         0x020
#define A_ESR           0x088
#define   ESR_ET_RD_DESC    0x0
#define   ESR_ET_WR_DESC    0x1
#define   ESR_ET_INTERPRET  0x2
#define   ESR_ET_RD_DATA    0x4
#define   ESR_ET_WR_DATA    0x5
#define   ESR_EIL           (1u << 12)
#define A_RCR           0x090
#define   RCR_RESET         0x60000000
#define A_RTC           0x0b4
#define   RTC_RESET         0x7ffc7ffc
#define A_TGC           0x300
#define   TGC_RESET         0x00222200
#define A_TCCR          0x304
#define   TCCR_TSRQ0        (1u << 0)
#define   TCCR_RW           0x000f0000
#define A_TSR           0x308
#define A_MFA           0x30c
#define A_MFA2          0x340
#define A_DIC           0x350
#define A_DIS           0x354
#define A_EIC           0x358
#define A_EIS           0x35c
#define   EIS_QEF           (1u << 2)
#define   EIS_QFS           (1u << 16)
#define   EIS_FLAGS         0x00000a07
#define A_RIC0          0x360
#define A_RIS0          0x364
#define   RIS0_FRF0         (1u << 0)
#define A_RIC1          0x368
#define A_RIS1          0x36c
#define A_RIC2          0x370
#define A_RIS2          0x374
#define   RIS2_QFF0         (1u << 0)
#define   RIS2_RFFF         (1u << 31)
#define   RIS2_FLAGS        (RIS2_QFF0 | RIS2_RFFF)
#define A_TIC           0x378
#define A_TIS           0x37c
#define   TIS_FTF0          (1u << 0)
#define   TIS_TDPF          (1u << 16)
#define   TIS_FLAGS         0x00017c01
#define A_ISS           0x380
#define   ISS_FRM           (1u << 0)
#define   ISS_FTM           (1u << 2)
#define   ISS_EM            (1u << 6)
#define   ISS_MM            (1u << 7)
#define   ISS_MFUM          (1u << 10)
#define   ISS_MFWM          (1u << 11)
#define   ISS_RFWM          (1u << 12)
#define   ISS_MFUM2         (1u << 14)
#define   ISS_MFWM2         (1u << 15)
#define   ISS_RCSRM         (1u << 16)
#define A_CIE           0x384
#define   CIE_CRIE          (1u << 0)
#define   CIE_CTIE          (1u << 8)
#define A_RIC3          0x388
#define A_RIS3          0x38c
#define   RIS3_RDPF         (1u << 0)
#define A_DIE           0x450
#define A_DID           0x454
#define A_EIE           0x458
#define A_EID           0x45c
#define A_RIE0          0x460
#define A_RID0          0x464
#define A_RIE1          0x468
#define A_RID1          0x46c
#define A_RIE2          0x470
#define A_RID2          0x474
#define A_TIE           0x478
#define A_TID           0x47c
#define A_RIE3          0x488
#define A_RID3          0x48c

/* E-MAC (Table 30.5) */
#define A_CXR20         0x500
#define   CXR20_PRM         (1u << 0)
#define   CXR20_TPE         (1u << 5)
#define   CXR20_RPE         (1u << 6)
#define   CXR20_RCPT        (1u << 25)
#define   CXR20_TRCCM       (1u << 26)
#define   CXR20_RW          0x07ff1073
#define A_CXR2A         0x508
#define   CXR2A_LEN_LMT     0x0003ffff
#define A_CXR21         0x510
#define   CXR21_FCI         (1u << 0)
#define   CXR21_LINKI       (1u << 2)
#define   CXR21_PHYI        (1u << 3)
#define   CXR21_PFRI        (1u << 4)
#define A_CXR22         0x518
#define   CXR22_RW          0x0000001d
#define A_CXR23         0x520
#define   CXR23_MDC         (1u << 0)
#define   CXR23_MM          (1u << 1)
#define   CXR23_MDO         (1u << 2)
#define   CXR23_MDI         (1u << 3)
#define A_CXR2B         0x528
#define   CXR2B_LINK        (1u << 0)
#define   CXR2B_INBAND      (1u << 1)
#define   CXR2B_SIGNAL      (1u << 2)
#define   CXR2B_SOFT        (1u << 3)
#define A_CXR2C         0x52c
#define A_CXR31         0x530
#define A_CXR35         0x540
#define   CXR35_RESET       0xffff0000
#define A_CXR71         0x554
#define A_CXR8A         0x55c
#define A_CXR80         0x560
#define A_CXR81         0x564
#define A_CXR2D         0x5b0
#define A_CXR2G         0x5bc
#define A_CXR24         0x5c0
#define A_CXR25         0x5c8
#define A_CXR40         0x700
#define A_CXR57         0x778
#define A_MDIOMOD       0x790
#define   MDIOMOD_RESET     0x800000ff
#define   MDIOMOD_SEL_MDIO_N (1u << 31)

/* TOE (Table 30.6) */
#define A_CSR0          0x800
#define   CSR0_TPE          (1u << 4)
#define   CSR0_RPE          (1u << 5)
#define   CSR0_TBP          (1u << 8)
#define   CSR0_RBP          (1u << 9)
#define   CSR0_RW           0x00000331
#define A_CSR1          0x804
#define A_CSR2          0x808
#define   CSR12_IP4         (1u << 0)
#define   CSR12_TCP4        (1u << 4)
#define   CSR12_UDP4        (1u << 5)
#define   CSR12_ICMP4       (1u << 6)
#define   CSR12_TCP6        (1u << 20)
#define   CSR12_UDP6        (1u << 21)
#define   CSR12_ICMP6       (1u << 22)
#define   CSR12_RW          0x0f700071

/* Descriptors (Table 30.7, Figure 30.7) */
#define DT_LEMPTY       0x2
#define DT_EEMPTY       0x3
#define DT_FMID         0x4
#define DT_FSTART       0x5
#define DT_FEND         0x6
#define DT_FSINGLE      0x7
#define DT_LINK         0x8
#define DT_LINKFIX      0x9
#define DT_EOS          0xa
#define DT_FEMPTY       0xc
#define DT_FEMPTY_ND    0xf
#define DESC_DS_MASK    0xfff
#define DESC_EI         (1u << 13)
#define MSC_CEEF        (1u << 6)
#define MSC_UNSUPPORTED (1u << 5)
#define MSC_MC          (1u << 7)

/* A queue walk ends after this many descriptors without progress */
#define GBETH_DESC_WALK_MAX 0x10000

enum {
    OPC_RESET = 0,
    OPC_CONFIG = 1,
    OPC_OPERATION = 2,
};

enum {
    QUEUE_TX = 0,
    QUEUE_RX = 4,
};

enum {
    MDIO_IDLE,
    MDIO_START,
    MDIO_CMD,
    MDIO_READ,
    MDIO_WRITE,
};

static unsigned gbeth_opc(RenesasGbEthState *s)
{
    return s->ccc & CCC_OPC_MASK;
}

/*
 * ISS (section 30.4.1.43). With @output, the terms of pif_int_n: CIE
 * gates the descriptor-processed flags of RIS3 and TIS there, and has
 * no influence on ISS itself (section 30.4.1.42, Table 30.20).
 */
static uint32_t gbeth_iss(RenesasGbEthState *s, bool output)
{
    uint32_t iss = 0;
    uint32_t tis = s->tis & s->tic;
    uint32_t ris3 = s->ris3 & s->ric3;

    if (output && !(s->cie & CIE_CTIE)) {
        tis &= ~TIS_TDPF;
    }
    if (output && !(s->cie & CIE_CRIE)) {
        ris3 &= ~RIS3_RDPF;
    }
    if ((s->ris0 & s->ric0 & RIS0_FRF0) || ris3) {
        iss |= ISS_FRM;
    }
    if (tis & (TIS_FTF0 | TIS_TDPF)) {
        iss |= ISS_FTM;
    }
    if ((s->eis & s->eic & EIS_FLAGS) || (s->eis & EIS_QFS)) {
        iss |= ISS_EM;
    }
    if (s->ecsr & s->ecsipr & CXR22_RW) {
        iss |= ISS_MM;
    }
    iss |= (tis >> 10 & 1) ? ISS_MFUM : 0;
    iss |= (tis >> 11 & 1) ? ISS_MFWM : 0;
    iss |= (tis >> 12 & 1) ? ISS_MFUM2 : 0;
    iss |= (tis >> 13 & 1) ? ISS_MFWM2 : 0;
    iss |= (tis >> 14 & 1) ? ISS_RCSRM : 0;
    /* DPM1-15: DIS.DPFn & DIC.DPEn, in bits 17-31 */
    iss |= (s->dis & s->dic & 0xfffe) << 16;
    return iss;
}

static void gbeth_update_irq(RenesasGbEthState *s)
{
    /* EIS.QFS summarises the queue-full and FIFO-full flags */
    if ((s->ris2 & s->ric2 & RIS2_FLAGS)) {
        s->eis |= EIS_QFS;
    } else {
        s->eis &= ~EIS_QFS;
    }
    qemu_set_irq(s->irq[0], gbeth_iss(s, true) != 0);
}

/* Record a queue error (ESR, EIS.QEF), section 30.4.1.6 */
static void gbeth_queue_error(RenesasGbEthState *s, int q, unsigned et,
                              hwaddr addr)
{
    qemu_log_mask(LOG_GUEST_ERROR, "renesas_gbeth: queue %d error %u at "
                  "0x%" HWADDR_PRIx "\n", q, et, addr);
    trace_renesas_gbeth_queue_error(q, et, addr);
    if (s->eis & EIS_QEF) {
        s->esr |= ESR_EIL;
    } else {
        s->esr = (et << 8) | q;
    }
    s->eis |= EIS_QEF;
}

/* The descriptor-processed interrupt of a DIE value (section 30.4.1.12) */
static void gbeth_desc_done(RenesasGbEthState *s, int q, unsigned die)
{
    if (die) {
        s->dis |= 1u << die;
        if (die == 1) {
            if (q == QUEUE_TX) {
                s->tis |= TIS_TDPF;
            } else {
                s->ris3 |= RIS3_RDPF;
            }
        }
    }
}

static bool gbeth_read_desc(RenesasGbEthState *s, int q, hwaddr addr,
                            uint32_t *w0, uint32_t *dptr)
{
    uint32_t d[2];

    if (dma_memory_read(&s->dma_as, addr, d, sizeof(d),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        gbeth_queue_error(s, q, ESR_ET_RD_DESC, addr);
        return false;
    }
    *w0 = le32_to_cpu(d[0]);
    *dptr = le32_to_cpu(d[1]);
    return true;
}

static bool gbeth_write_w0(RenesasGbEthState *s, int q, hwaddr addr,
                           uint32_t w0)
{
    uint32_t le = cpu_to_le32(w0);

    if (dma_memory_write(&s->dma_as, addr, &le, sizeof(le),
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        gbeth_queue_error(s, q, ESR_ET_WR_DESC, addr);
        return false;
    }
    return true;
}

static uint32_t gbeth_set_dt(uint32_t w0, unsigned dt)
{
    return deposit32(w0, 28, 4, dt);
}

/*
 * TOE checksums. Returns the L4 header offset and protocol of an
 * unfragmented IPv4 (without options) or IPv6 (without extension
 * headers) frame, behind at most one C-tag; 0 when not supported.
 */
static size_t toe_parse(const uint8_t *buf, size_t len, bool *v6,
                        uint8_t *proto, size_t *ip_off, size_t *l4_len)
{
    size_t off = ETH_HLEN;
    uint16_t type;

    if (len < ETH_HLEN) {
        return 0;
    }
    type = lduw_be_p(buf + 12);
    if (type == ETH_P_VLAN) {
        if (len < ETH_HLEN + 4) {
            return 0;
        }
        type = lduw_be_p(buf + 16);
        off += 4;
    }
    *ip_off = off;
    if (type == ETH_P_IP) {
        uint16_t tot, frag;

        if (len < off + 20 || (buf[off] >> 4) != 4 || (buf[off] & 0xf) != 5) {
            return 0;
        }
        tot = lduw_be_p(buf + off + 2);
        frag = lduw_be_p(buf + off + 6);
        if (tot < 20 || len < off + tot || (frag & 0x3fff)) {
            return 0;
        }
        *v6 = false;
        *proto = buf[off + 9];
        *l4_len = tot - 20;
        return off + 20;
    }
    if (type == ETH_P_IPV6) {
        uint16_t plen;

        if (len < off + 40 || (buf[off] >> 4) != 6) {
            return 0;
        }
        plen = lduw_be_p(buf + off + 4);
        if (len < off + 40 + plen) {
            return 0;
        }
        *v6 = true;
        *proto = buf[off + 6];
        *l4_len = plen;
        return off + 40;
    }
    return 0;
}

static bool toe_l4_enabled(uint32_t csr, bool v6, uint8_t proto)
{
    switch (proto) {
    case IP_PROTO_TCP:
        return csr & (v6 ? CSR12_TCP6 : CSR12_TCP4);
    case IP_PROTO_UDP:
        return csr & (v6 ? CSR12_UDP6 : CSR12_UDP4);
    case IPPROTO_ICMP:
        return !v6 && (csr & CSR12_ICMP4);
    case IPPROTO_ICMPV6:
        return v6 && (csr & CSR12_ICMP6);
    default:
        return false;
    }
}

static size_t toe_l4_csum_off(uint8_t proto)
{
    switch (proto) {
    case IP_PROTO_TCP:
        return 16;
    case IP_PROTO_UDP:
        return 6;
    default:
        return 2;   /* ICMP, ICMPv6 */
    }
}

/*
 * The checksum of an L4 segment and its pseudo-header: the value to
 * store in a segment whose checksum field is 0, and 0 when a received
 * segment is correct.
 */
static uint16_t toe_l4_csum(uint8_t *buf, size_t ip_off, size_t l4_off,
                            size_t l4_len, bool v6, uint8_t proto)
{
    uint32_t sum = 0;

    if (v6) {
        sum = net_checksum_add(32, buf + ip_off + 8);
        sum += l4_len + proto;
    } else if (proto != IPPROTO_ICMP) {
        sum = net_checksum_add(8, buf + ip_off + 12);
        sum += l4_len + proto;
    }
    sum += net_checksum_add(l4_len, buf + l4_off);
    return net_checksum_finish(sum);
}

static void toe_tx(RenesasGbEthState *s, uint8_t *buf, size_t len)
{
    size_t ip_off, l4_off, l4_len, csum_off;
    uint8_t proto;
    bool v6;

    l4_off = toe_parse(buf, len, &v6, &proto, &ip_off, &l4_len);
    if (!l4_off) {
        return;
    }
    if (!v6 && (s->csr1 & CSR12_IP4)) {
        stw_be_p(buf + ip_off + 10, 0);
        stw_be_p(buf + ip_off + 10, net_raw_checksum(buf + ip_off, 20));
    }
    if (!toe_l4_enabled(s->csr1, v6, proto)) {
        return;
    }
    csum_off = l4_off + toe_l4_csum_off(proto);
    if (l4_len < toe_l4_csum_off(proto) + 2) {
        return;
    }
    /* An IPv4 UDP checksum of 0 means "none": it is left at 0 */
    if (!v6 && proto == IP_PROTO_UDP && !lduw_be_p(buf + csum_off)) {
        return;
    }
    stw_be_p(buf + csum_off, 0);
    stw_be_p(buf + csum_off,
             toe_l4_csum(buf, ip_off, l4_off, l4_len, v6, proto));
    /* A UDP checksum that computes to 0 is sent as H'FFFF (RFC 768) */
    if (proto == IP_PROTO_UDP && !lduw_be_p(buf + csum_off)) {
        stw_be_p(buf + csum_off, 0xffff);
    }
}

/* The 4-byte reception result (Figure 30.25); returns MSC bits */
static uint8_t toe_rx(RenesasGbEthState *s, uint8_t *buf, size_t len,
                      uint8_t *result)
{
    size_t ip_off, l4_off, l4_len, csum_off;
    uint16_t ip_res = 0xffff, l4_res = 0xffff;
    uint8_t proto, msc = 0;
    bool v6;

    l4_off = toe_parse(buf, len, &v6, &proto, &ip_off, &l4_len);
    if (l4_off && !v6 && (s->csr2 & CSR12_IP4)) {
        ip_res = net_raw_checksum(buf + ip_off, 20);
    }
    if (l4_off && toe_l4_enabled(s->csr2, v6, proto) &&
        l4_len >= toe_l4_csum_off(proto) + 2) {
        csum_off = l4_off + toe_l4_csum_off(proto);
        if (!v6 && proto == IP_PROTO_UDP && !lduw_be_p(buf + csum_off)) {
            l4_res = 0;
        } else {
            l4_res = toe_l4_csum(buf, ip_off, l4_off, l4_len, v6, proto);
        }
    }
    if ((ip_res && ip_res != 0xffff) || (l4_res && l4_res != 0xffff)) {
        msc |= MSC_CEEF;
    } else if (ip_res == 0xffff && l4_res == 0xffff) {
        msc |= MSC_UNSUPPORTED;
    }
    stw_le_p(result, ip_res);
    stw_le_p(result + 2, l4_res);
    return msc;
}

static bool gbeth_link_selected(RenesasGbEthState *s)
{
    switch ((s->cxr31 & 1) | (s->cxr31 >> 2 & 2)) {
    case 0:     /* LINKSTA pin */
    case 1:     /* RGMII in-band status */
        return s->phy_link;
    case 2:
        return s->cxr2g & 1;
    default:
        return s->ecmr & CXR20_TPE;
    }
}

/* CXR21.LINKI: a change of the link CXR2B.LINK reports */
static void gbeth_update_link(RenesasGbEthState *s)
{
    bool link = gbeth_link_selected(s);

    if (link != s->link) {
        s->link = link;
        s->ecsr |= CXR21_LINKI;
    }
}

static void gbeth_tx_send(RenesasGbEthState *s)
{
    if (s->tx_oversize) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_gbeth: transmission frame "
                      "longer than %d bytes dropped\n",
                      RENESAS_GBETH_TX_FRAME_MAX);
        return;
    }
    if ((s->csr0 & CSR0_TPE) && !(s->csr0 & CSR0_TBP)) {
        toe_tx(s, s->tx_frame, s->tx_len);
    }
    trace_renesas_gbeth_tx(s->tx_len);
    /* Without a link the frame leaves the MAC and is lost on the wire */
    if (s->phy_link) {
        qemu_send_packet(qemu_get_queue(s->nic), s->tx_frame, s->tx_len);
    }
}

/* Transmission queue (Table 30.14), run on TCCR.TSRQ0 */
static void gbeth_tx(RenesasGbEthState *s)
{
    int n;

    for (n = 0; n < GBETH_DESC_WALK_MAX; n++) {
        hwaddr addr = s->cdar_tx;
        uint32_t w0, dptr, ds;
        unsigned dt, die;

        if (!gbeth_read_desc(s, QUEUE_TX, addr, &w0, &dptr)) {
            break;
        }
        dt = extract32(w0, 28, 4);
        die = extract32(w0, 24, 4);
        ds = w0 & DESC_DS_MASK;
        switch (dt) {
        case DT_FSTART:
        case DT_FSINGLE:
        case DT_FMID:
        case DT_FEND:
            if (dt == DT_FSTART || dt == DT_FSINGLE) {
                s->tx_len = 0;
                s->tx_oversize = false;
                s->tx_in_frame = true;
            }
            if (s->tx_len + ds > RENESAS_GBETH_TX_FRAME_MAX) {
                s->tx_oversize = true;
            } else if (dma_memory_read(&s->dma_as, dptr,
                                       s->tx_frame + s->tx_len, ds,
                                       MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                gbeth_queue_error(s, QUEUE_TX, ESR_ET_RD_DATA, dptr);
                s->tx_in_frame = false;
                goto stop;
            } else {
                s->tx_len += ds;
            }
            if (!gbeth_write_w0(s, QUEUE_TX, addr,
                                gbeth_set_dt(w0, DT_FEMPTY))) {
                s->tx_in_frame = false;
                goto stop;
            }
            gbeth_desc_done(s, QUEUE_TX, die);
            s->cdar_tx += 8;
            if ((dt == DT_FEND || dt == DT_FSINGLE) && s->tx_in_frame) {
                gbeth_tx_send(s);
                s->tx_in_frame = false;
                s->tis |= TIS_FTF0;
            }
            break;
        case DT_LINK:
            if (!gbeth_write_w0(s, QUEUE_TX, addr,
                                gbeth_set_dt(w0, DT_LEMPTY))) {
                goto stop;
            }
            gbeth_desc_done(s, QUEUE_TX, die);
            s->cdar_tx = dptr;
            break;
        case DT_LINKFIX:
            s->cdar_tx = dptr;
            break;
        case DT_EOS:
            /* A stop point: the next request starts at the next one */
            if (gbeth_write_w0(s, QUEUE_TX, addr,
                               gbeth_set_dt(w0, DT_EEMPTY))) {
                gbeth_desc_done(s, QUEUE_TX, die);
                s->cdar_tx += 8;
            }
            goto stop;
        case DT_FEMPTY:
        case DT_LEMPTY:
        case DT_EEMPTY:
            /* Owned by the software: fetched again on the next request */
            goto stop;
        default:
            s->tx_in_frame = false;
            gbeth_queue_error(s, QUEUE_TX, ESR_ET_INTERPRET, addr);
            goto stop;
        }
    }
    if (n == GBETH_DESC_WALK_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_gbeth: transmission queue "
                      "loops without an empty descriptor\n");
    }
stop:
    s->tccr &= ~TCCR_TSRQ0;
}

static void gbeth_tx_kick(RenesasGbEthState *s)
{
    if ((s->tccr & TCCR_TSRQ0) && (s->ecmr & CXR20_TPE) &&
        gbeth_opc(s) == OPC_OPERATION) {
        gbeth_tx(s);
    }
}

static bool gbeth_rx_filter(RenesasGbEthState *s, const uint8_t *buf)
{
    uint8_t mac[6];

    if ((s->ecmr & CXR20_PRM) || (buf[0] & 1)) {
        return true;
    }
    stl_be_p(mac, s->mahr);
    stw_be_p(mac + 4, s->malr);
    return !memcmp(buf, mac, 6);
}

static uint32_t gbeth_rx_max(RenesasGbEthState *s)
{
    uint32_t lmt = s->rflr & CXR2A_LEN_LMT;

    return lmt <= 1518 ? 1518 : lmt;
}

static bool gbeth_can_receive(NetClientState *nc)
{
    return true;
}

static ssize_t gbeth_receive(NetClientState *nc, const uint8_t *data,
                             size_t size)
{
    RenesasGbEthState *s = qemu_get_nic_opaque(nc);
    g_autofree uint8_t *frame = NULL;
    size_t len = MAX(size, ETH_ZLEN), done = 0;
    uint8_t msc = 0;
    bool start = true;
    int n;

    if (s->in_reset || gbeth_opc(s) != OPC_OPERATION ||
        !(s->ecmr & CXR20_RPE) || !s->phy_link || size < ETH_HLEN ||
        !gbeth_rx_filter(s, data)) {
        return size;
    }
    /* The length check includes the CRC */
    if (len + 4 > gbeth_rx_max(s) || len + 4 > RENESAS_GBETH_RX_FRAME_MAX) {
        return size;
    }
    frame = g_malloc0(len + 4);
    memcpy(frame, data, size);
    if (s->ecmr & CXR20_RCPT) {
        if ((s->csr0 & CSR0_RPE) && !(s->csr0 & CSR0_RBP)) {
            msc |= toe_rx(s, frame, len, frame + len);
        } else {
            stl_le_p(frame + len, crc32(0, frame, len));
        }
        len += 4;
    }
    /* Multicast, not broadcast: the manual counts them apart (I) */
    if ((data[0] & 1) && !is_broadcast_ether_addr(data)) {
        msc |= MSC_MC;
        s->mc_count++;
    }
    trace_renesas_gbeth_rx(size, msc);

    for (n = 0; n < GBETH_DESC_WALK_MAX && done < len; n++) {
        hwaddr addr = s->cdar_rx;
        uint32_t w0, dptr, ds, part;
        unsigned dt, die, wdt;

        if (!gbeth_read_desc(s, QUEUE_RX, addr, &w0, &dptr)) {
            return size;
        }
        dt = extract32(w0, 28, 4);
        die = extract32(w0, 24, 4);
        switch (dt) {
        case DT_FEMPTY:
        case DT_FEMPTY_ND:
            ds = w0 & DESC_DS_MASK;
            part = MIN(ds, len - done);
            if (!part && ds) {
                break;
            }
            if (dt == DT_FEMPTY && part &&
                dma_memory_write(&s->dma_as, dptr, frame + done, part,
                                 MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                /*
                 * Section 30.5.5.4: the descriptor is written back with
                 * EI and CDAR moves on; the frame is lost
                 */
                gbeth_queue_error(s, QUEUE_RX, ESR_ET_WR_DATA, dptr);
                w0 = ((start ? DT_FSINGLE : DT_FEND) << 28) | (die << 24) |
                     (msc << 16) | DESC_EI;
                if (gbeth_write_w0(s, QUEUE_RX, addr, w0)) {
                    s->cdar_rx += 8;
                }
                gbeth_update_irq(s);
                return size;
            }
            done += part;
            if (start) {
                wdt = done == len ? DT_FSINGLE : DT_FSTART;
            } else {
                wdt = done == len ? DT_FEND : DT_FMID;
            }
            start = false;
            w0 = (wdt << 28) | (die << 24) | (msc << 16) |
                 (dt == DT_FEMPTY ? part : 0);
            if (!gbeth_write_w0(s, QUEUE_RX, addr, w0)) {
                return size;
            }
            gbeth_desc_done(s, QUEUE_RX, die);
            s->cdar_rx += 8;
            break;
        case DT_LINK:
            if (!gbeth_write_w0(s, QUEUE_RX, addr,
                                gbeth_set_dt(w0, DT_LEMPTY))) {
                return size;
            }
            gbeth_desc_done(s, QUEUE_RX, die);
            s->cdar_rx = dptr;
            break;
        case DT_LINKFIX:
            s->cdar_rx = dptr;
            break;
        case DT_EOS:
            if (!gbeth_write_w0(s, QUEUE_RX, addr,
                                gbeth_set_dt(w0, DT_EEMPTY))) {
                return size;
            }
            gbeth_desc_done(s, QUEUE_RX, die);
            s->cdar_rx += 8;
            if (!start) {
                /* An end of set inside a frame aborts it */
                s->ris2 |= RIS2_QFF0;
                gbeth_update_irq(s);
                return size;
            }
            break;
        case DT_FMID:
        case DT_FSTART:
        case DT_FEND:
        case DT_FSINGLE:
        case DT_LEMPTY:
        case DT_EEMPTY:
            /* Queue full: the frame is discarded */
            s->ris2 |= RIS2_QFF0;
            gbeth_update_irq(s);
            return size;
        default:
            gbeth_queue_error(s, QUEUE_RX, ESR_ET_INTERPRET, addr);
            gbeth_update_irq(s);
            return size;
        }
    }
    if (done < len) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_gbeth: reception queue "
                      "loops without an empty descriptor\n");
        return size;
    }
    s->ris0 |= RIS0_FRF0;
    gbeth_update_irq(s);
    return size;
}

static void gbeth_set_link(NetClientState *nc)
{
    RenesasGbEthState *s = qemu_get_nic_opaque(nc);

    if (s->phy) {
        ksz9131_phy_set_cable(s->phy, !nc->link_down && nc->peer);
    }
}

/* DMAC operating mode (section 30.5.1) */
static void gbeth_clear_status(RenesasGbEthState *s)
{
    s->esr = 0;
    s->dis = 0;
    s->eis = 0;
    s->ris0 = 0;
    s->ris2 = 0;
    s->ris3 = 0;
    s->tis = 0;
    s->tccr &= ~TCCR_TSRQ0;
    s->tx_in_frame = false;
}

static void gbeth_write_ccc(RenesasGbEthState *s, uint32_t val)
{
    unsigned old = gbeth_opc(s), opc = val & CCC_OPC_MASK;

    if (opc == 3) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_gbeth: CCC.OPC 3 is "
                      "reserved\n");
        opc = old;
    }
    s->ccc = (val & CCC_RW & ~CCC_OPC_MASK) | opc;
    if (opc == old) {
        return;
    }
    trace_renesas_gbeth_opmode(old, opc);
    if (old == OPC_OPERATION || opc == OPC_CONFIG) {
        gbeth_clear_status(s);
    }
    if (old == OPC_OPERATION) {
        s->cdar_tx = 0;
        s->cdar_rx = 0;
    }
    if (opc == OPC_OPERATION) {
        s->cdar_tx = s->dbat + 8 * QUEUE_TX;
        s->cdar_rx = s->dbat + 8 * QUEUE_RX;
    }
}

/* CXR23: MDC rising edges clock a clause 22 frame (IEEE 802.3 22.2.4.5) */
static void gbeth_mdio_clock(RenesasGbEthState *s)
{
    bool drive = s->pir & CXR23_MM;
    bool bit = s->pir & CXR23_MDO;

    switch (s->mdio_state) {
    case MDIO_IDLE:
        if (drive) {
            if (bit) {
                s->mdio_bits = 1;
            } else if (s->mdio_bits) {
                s->mdio_state = MDIO_START;
            }
        }
        break;
    case MDIO_START:
        s->mdio_state = (drive && bit) ? MDIO_CMD : MDIO_IDLE;
        s->mdio_bits = 0;
        s->mdio_shift = 0;
        break;
    case MDIO_CMD: {
        unsigned op;

        s->mdio_shift = (s->mdio_shift << 1) | bit;
        if (++s->mdio_bits < 12) {
            break;
        }
        /* OP[1:0], PHYAD[4:0], REGAD[4:0] */
        op = s->mdio_shift >> 10;
        s->mdio_reg = s->mdio_shift & 0x1f;
        s->mdio_respond = s->phy &&
                          ((s->mdio_shift >> 5) & 0x1f) == s->phy_addr;
        s->mdio_bits = 0;
        s->mdio_shift = 0;
        if (op == 2) {
            s->mdio_state = MDIO_READ;
            if (s->mdio_respond) {
                s->mdio_data = ksz9131_phy_read(s->phy, s->mdio_reg);
                trace_renesas_gbeth_mdio(false, s->mdio_reg, s->mdio_data);
            }
        } else if (op == 1) {
            s->mdio_state = MDIO_WRITE;
        } else {
            s->mdio_state = MDIO_IDLE;
        }
        break;
    }
    case MDIO_READ:
        s->mdio_bits++;
        if (s->mdio_bits == 1) {
            s->mdio_out = !s->mdio_respond;     /* turnaround: 0 */
        } else if (s->mdio_bits <= 17) {
            s->mdio_out = !s->mdio_respond ||
                          (s->mdio_data >> (17 - s->mdio_bits) & 1);
        } else {
            s->mdio_out = true;
            s->mdio_state = MDIO_IDLE;
            s->mdio_bits = 0;
        }
        break;
    case MDIO_WRITE:
        s->mdio_bits++;
        if (s->mdio_bits > 2) {
            s->mdio_shift = (s->mdio_shift << 1) | bit;
        }
        if (s->mdio_bits == 18) {
            if (s->mdio_respond) {
                trace_renesas_gbeth_mdio(true, s->mdio_reg, s->mdio_shift);
                ksz9131_phy_write(s->phy, s->mdio_reg, s->mdio_shift);
            }
            s->mdio_state = MDIO_IDLE;
            s->mdio_bits = 0;
        }
        break;
    }
}

static bool gbeth_is_counter(hwaddr offset)
{
    return offset == A_CXR8A || offset == A_CXR80 ||
           (offset >= A_CXR40 && offset <= A_CXR57 && !(offset & 7) &&
            (offset <= 0x710 || offset >= 0x740));
}

/* Frame counters: only CXR57 counts (multicast frames received) */
static uint32_t gbeth_counter(RenesasGbEthState *s, hwaddr offset)
{
    return offset == A_CXR57 ? s->mc_count : 0;
}

static void gbeth_counter_clear(RenesasGbEthState *s, hwaddr offset)
{
    if (offset == A_CXR57) {
        s->mc_count = 0;
    }
}

static MemTxResult gbeth_read(void *opaque, hwaddr offset, uint64_t *data,
                              unsigned size, MemTxAttrs attrs)
{
    RenesasGbEthState *s = RENESAS_GBETH(opaque);
    uint32_t v;

    switch (offset) {
    case A_CCC:
        v = s->ccc;
        break;
    case A_DBAT:
        v = s->dbat;
        break;
    case A_DLR:
        v = DLR_RESET;
        break;
    case A_CSR:
        /* OPS; TPO and RPO stay 0: transfers complete when requested */
        v = 1u << gbeth_opc(s);
        break;
    case A_CDAR0:
        v = s->cdar_tx;
        break;
    case A_CDAR4:
        v = s->cdar_rx;
        break;
    case A_ESR:
        v = s->esr;
        break;
    case A_RCR:
        v = s->rcr;
        break;
    case A_RTC:
        v = s->rtc;
        break;
    case A_TGC:
        v = s->tgc;
        break;
    case A_TCCR:
        v = s->tccr;
        break;
    case A_TSR:
    case A_MFA:
    case A_MFA2:
    case A_RIS1:
        v = 0;
        break;
    case A_DIC:
        v = s->dic;
        break;
    case A_DIS:
        v = s->dis;
        break;
    case A_EIC:
        v = s->eic;
        break;
    case A_EIS:
        v = s->eis;
        break;
    case A_RIC0:
        v = s->ric0;
        break;
    case A_RIS0:
        v = s->ris0;
        break;
    case A_RIC1:
        v = s->ric1;
        break;
    case A_RIC2:
        v = s->ric2;
        break;
    case A_RIS2:
        v = s->ris2;
        break;
    case A_TIC:
        v = s->tic;
        break;
    case A_TIS:
        v = s->tis;
        break;
    case A_ISS:
        v = gbeth_iss(s, false);
        break;
    case A_CIE:
        v = s->cie;
        break;
    case A_RIC3:
        v = s->ric3;
        break;
    case A_RIS3:
        v = s->ris3;
        break;
    case A_DIE ... A_RID2:
    case A_TIE:
    case A_TID:
    case A_RIE3:
    case A_RID3:
        v = 0;
        break;
    case A_CXR20:
        v = s->ecmr;
        break;
    case A_CXR2A:
        v = s->rflr;
        break;
    case A_CXR21:
        v = s->ecsr;
        break;
    case A_CXR22:
        v = s->ecsipr;
        break;
    case A_CXR23:
        v = s->pir & (CXR23_MDC | CXR23_MM | CXR23_MDO);
        if (s->pir & CXR23_MM) {
            v |= (s->pir & CXR23_MDO) ? CXR23_MDI : 0;
        } else {
            v |= s->mdio_out ? CXR23_MDI : 0;
        }
        break;
    case A_CXR2B:
        v = (s->link ? CXR2B_LINK : 0) |
            (s->phy_link ? CXR2B_INBAND | CXR2B_SIGNAL : 0) |
            ((s->cxr2g & 1) ? CXR2B_SOFT : 0);
        break;
    case A_CXR2C:
        v = s->pipr;
        break;
    case A_CXR31:
        v = s->cxr31;
        break;
    case A_CXR35:
        v = s->cxr35;
        break;
    case A_CXR71:
        v = s->cxr71;
        break;
    case A_CXR81:
        v = s->cxr81;
        break;
    case A_CXR2D:
        v = s->gecmr;
        break;
    case A_CXR2G:
        v = s->cxr2g;
        break;
    case A_CXR24:
        v = s->mahr;
        break;
    case A_CXR25:
        v = s->malr;
        break;
    case A_MDIOMOD:
        v = s->mdiomod;
        break;
    case A_CSR0:
        v = s->csr0;
        break;
    case A_CSR1:
        v = s->csr1;
        break;
    case A_CSR2:
        v = s->csr2;
        break;
    default:
        if (gbeth_is_counter(offset)) {
            v = gbeth_counter(s, offset);
            if (s->ecmr & CXR20_TRCCM) {
                gbeth_counter_clear(s, offset);
            }
            break;
        }
        qemu_log_mask(LOG_UNIMP, "renesas_gbeth: unimplemented read at "
                      "offset 0x%" HWADDR_PRIx "\n", offset);
        return MEMTX_DECODE_ERROR;
    }
    trace_renesas_gbeth_read(offset, v);
    *data = v;
    return MEMTX_OK;
}

static MemTxResult gbeth_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    RenesasGbEthState *s = RENESAS_GBETH(opaque);
    uint32_t v = value;
    bool pir_rise;

    trace_renesas_gbeth_write(offset, v);
    if (s->in_reset) {
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_gbeth: write at offset 0x%"
                      HWADDR_PRIx " in reset ignored\n", offset);
        return MEMTX_OK;
    }
    switch (offset) {
    case A_CCC:
        gbeth_write_ccc(s, v);
        break;
    case A_DBAT:
        /* Writable in CONFIG and OPERATION, a multiple of 4 (30.4.1.2) */
        if (gbeth_opc(s) == OPC_RESET) {
            qemu_log_mask(LOG_GUEST_ERROR, "renesas_gbeth: DBAT write in "
                          "RESET mode ignored\n");
        } else {
            s->dbat = v & ~3u;
        }
        break;
    case A_CSR:
    case A_CDAR0:
    case A_CDAR4:
    case A_ESR:
    case A_TSR:
    case A_MFA:
    case A_MFA2:
    case A_ISS:
    case A_CXR2B:
        qemu_log_mask(LOG_GUEST_ERROR, "renesas_gbeth: write to read-only "
                      "offset 0x%" HWADDR_PRIx " ignored\n", offset);
        break;
    case A_RCR:
        s->rcr = v & 0x7fff0001;
        break;
    case A_RTC:
        s->rtc = (v & 0x00037fff) | (RTC_RESET & ~0x00037fff);
        break;
    case A_TGC:
        s->tgc = (v & 0x00000300) | (TGC_RESET & ~0x00000300);
        break;
    case A_TCCR:
        s->tccr = (s->tccr & TCCR_TSRQ0) | (v & TCCR_RW);
        /* TSRQ0: only a 1 can be written, in OPERATION mode */
        if ((v & TCCR_TSRQ0) && gbeth_opc(s) == OPC_OPERATION) {
            s->tccr |= TCCR_TSRQ0;
            gbeth_tx_kick(s);
        }
        break;
    case A_DIC:
        s->dic = v & 0xfffe;
        break;
    case A_DIS:
        s->dis &= v | ~0xfffe;
        break;
    case A_EIC:
        s->eic = v & EIS_FLAGS;
        break;
    case A_EIS:
        s->eis &= v | ~EIS_FLAGS;
        if (!(s->eis & EIS_QEF)) {
            s->esr &= ~ESR_EIL;
        }
        break;
    case A_RIC0:
        s->ric0 = v & RIS0_FRF0;
        break;
    case A_RIS0:
        s->ris0 &= v | ~RIS0_FRF0;
        break;
    case A_RIC1:
        s->ric1 = v & (1u << 31);
        break;
    case A_RIS1:
        break;
    case A_RIC2:
        s->ric2 = v & RIS2_FLAGS;
        break;
    case A_RIS2:
        s->ris2 &= v | ~RIS2_FLAGS;
        break;
    case A_TIC:
        s->tic = v & TIS_FLAGS;
        break;
    case A_TIS:
        s->tis &= v | ~TIS_FLAGS;
        break;
    case A_CIE:
        s->cie = v & (CIE_CRIE | CIE_CTIE);
        break;
    case A_RIC3:
        s->ric3 = v & RIS3_RDPF;
        break;
    case A_RIS3:
        s->ris3 &= v | ~RIS3_RDPF;
        break;
    case A_DIE:
        s->dic |= v & 0xfffe;
        break;
    case A_DID:
        s->dic &= ~v;
        break;
    case A_EIE:
        s->eic |= v & EIS_FLAGS;
        break;
    case A_EID:
        s->eic &= ~v;
        break;
    case A_RIE0:
        s->ric0 |= v & RIS0_FRF0;
        break;
    case A_RID0:
        s->ric0 &= ~v;
        break;
    case A_RIE1:
        s->ric1 |= v & (1u << 31);
        break;
    case A_RID1:
        s->ric1 &= ~v;
        break;
    case A_RIE2:
        s->ric2 |= v & RIS2_FLAGS;
        break;
    case A_RID2:
        s->ric2 &= ~v;
        break;
    case A_TIE:
        s->tic |= v & TIS_FLAGS;
        break;
    case A_TID:
        s->tic &= ~v;
        break;
    case A_RIE3:
        s->ric3 |= v & RIS3_RDPF;
        break;
    case A_RID3:
        s->ric3 &= ~v;
        break;
    case A_CXR20:
        s->ecmr = v & CXR20_RW;
        gbeth_update_link(s);
        gbeth_tx_kick(s);
        break;
    case A_CXR2A:
        s->rflr = v & CXR2A_LEN_LMT;
        break;
    case A_CXR21:
        /* FCI, LINKI, PFRI: a 1 clears; PHYI follows its pin */
        s->ecsr &= ~(v & (CXR21_FCI | CXR21_LINKI | CXR21_PFRI));
        break;
    case A_CXR22:
        s->ecsipr = v & CXR22_RW;
        break;
    case A_CXR23:
        pir_rise = !(s->pir & CXR23_MDC) && (v & CXR23_MDC);
        s->pir = v & (CXR23_MDC | CXR23_MM | CXR23_MDO);
        if (pir_rise) {
            gbeth_mdio_clock(s);
        }
        break;
    case A_CXR2C:
        s->pipr = v & 1;
        break;
    case A_CXR31:
        s->cxr31 = v & 0xf;
        gbeth_update_link(s);
        break;
    case A_CXR35:
        s->cxr35 = v & 0xffff1103;
        break;
    case A_CXR71:
        s->cxr71 = v & 0xffff;
        break;
    case A_CXR81:
        s->cxr81 = v & 0xffff;
        break;
    case A_CXR2D:
        s->gecmr = v & 0x30;
        break;
    case A_CXR2G:
        s->cxr2g = v & 1;
        gbeth_update_link(s);
        break;
    case A_CXR24:
        s->mahr = v;
        break;
    case A_CXR25:
        s->malr = v & 0xffff;
        break;
    case A_MDIOMOD:
        if (!(v & MDIOMOD_SEL_MDIO_N)) {
            qemu_log_mask(LOG_UNIMP, "renesas_gbeth: the MDIO controller "
                          "is not modelled\n");
            return MEMTX_DECODE_ERROR;
        }
        s->mdiomod = v & (MDIOMOD_SEL_MDIO_N | 0xff);
        break;
    case A_CSR0:
        s->csr0 = v & CSR0_RW;
        break;
    case A_CSR1:
        s->csr1 = v & CSR12_RW;
        break;
    case A_CSR2:
        s->csr2 = v & CSR12_RW;
        break;
    default:
        if (gbeth_is_counter(offset)) {
            /* Cleared by any write while CXR20.TRCCM = 0 */
            if (!(s->ecmr & CXR20_TRCCM)) {
                gbeth_counter_clear(s, offset);
            }
            break;
        }
        qemu_log_mask(LOG_UNIMP, "renesas_gbeth: unimplemented write at "
                      "offset 0x%" HWADDR_PRIx "\n", offset);
        return MEMTX_DECODE_ERROR;
    }
    gbeth_update_irq(s);
    return MEMTX_OK;
}

static const MemoryRegionOps gbeth_ops = {
    .read_with_attrs = gbeth_read,
    .write_with_attrs = gbeth_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void gbeth_reset_regs(RenesasGbEthState *s)
{
    s->ccc = 0;
    s->dbat = 0;
    s->cdar_tx = 0;
    s->cdar_rx = 0;
    s->rcr = RCR_RESET;
    s->rtc = RTC_RESET;
    s->tgc = TGC_RESET;
    s->tccr = 0;
    s->dic = 0;
    s->eic = 0;
    s->ric0 = 0;
    s->ric1 = 0;
    s->ric2 = 0;
    s->tic = 0;
    s->cie = 0;
    s->ric3 = 0;
    gbeth_clear_status(s);
    s->ecmr = 0;
    s->rflr = 0;
    s->ecsr = 0;
    s->ecsipr = 0;
    s->pir = 0;
    s->pipr = 0;
    s->cxr31 = 0;
    s->cxr35 = CXR35_RESET;
    s->cxr71 = 0;
    s->cxr81 = 0;
    s->gecmr = 0;
    s->cxr2g = 0;
    s->mahr = 0;
    s->malr = 0;
    s->mdiomod = MDIOMOD_RESET;
    s->mc_count = 0;
    s->csr0 = 0;
    s->csr1 = 0;
    s->csr2 = 0;
    s->mdio_state = MDIO_IDLE;
    s->mdio_bits = 0;
    s->mdio_out = true;
    s->link = gbeth_link_selected(s);
}

static void gbeth_reset_hold(Object *obj, ResetType type)
{
    RenesasGbEthState *s = RENESAS_GBETH(obj);

    gbeth_reset_regs(s);
    gbeth_update_irq(s);
}

static void gbeth_reset_in(void *opaque, int n, int level)
{
    RenesasGbEthState *s = RENESAS_GBETH(opaque);

    trace_renesas_gbeth_reset(level);
    s->in_reset = level;
    if (s->in_reset) {
        gbeth_reset_regs(s);
        gbeth_update_irq(s);
    } else {
        /* The PHY may have changed the link while the reset was held */
        s->link = gbeth_link_selected(s);
    }
}

static void gbeth_phy_link(void *opaque, int n, int level)
{
    RenesasGbEthState *s = RENESAS_GBETH(opaque);

    s->phy_link = level;
    if (!s->in_reset) {
        gbeth_update_link(s);
        gbeth_update_irq(s);
    }
}

static NetClientInfo gbeth_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = gbeth_can_receive,
    .receive = gbeth_receive,
    .link_status_changed = gbeth_set_link,
};

static void gbeth_init(Object *obj)
{
    RenesasGbEthState *s = RENESAS_GBETH(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &gbeth_ops, s, TYPE_RENESAS_GBETH,
                          RENESAS_GBETH_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (i = 0; i < RENESAS_GBETH_NUM_IRQ; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), gbeth_reset_in, "reset", 1);
    qdev_init_gpio_in_named(DEVICE(obj), gbeth_phy_link, "phy-link", 1);
}

static void gbeth_realize(DeviceState *dev, Error **errp)
{
    RenesasGbEthState *s = RENESAS_GBETH(dev);
    NetClientState *nc;

    if (!s->dma_mr) {
        error_setg(errp, TYPE_RENESAS_GBETH " 'dma-memory' link not set");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "renesas-gbeth-dma");
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&gbeth_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    nc = qemu_get_queue(s->nic);
    qemu_format_nic_info_str(nc, s->conf.macaddr.a);
    /* No backend: no cable */
    if (s->phy) {
        ksz9131_phy_set_cable(s->phy, nc->peer && !nc->link_down);
    }
}

static void gbeth_unrealize(DeviceState *dev)
{
    RenesasGbEthState *s = RENESAS_GBETH(dev);

    qemu_del_nic(s->nic);
}

static const VMStateDescription vmstate_gbeth = {
    .name = TYPE_RENESAS_GBETH,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(in_reset, RenesasGbEthState),
        VMSTATE_BOOL(phy_link, RenesasGbEthState),
        VMSTATE_BOOL(link, RenesasGbEthState),
        VMSTATE_UINT32(ccc, RenesasGbEthState),
        VMSTATE_UINT32(dbat, RenesasGbEthState),
        VMSTATE_UINT32(cdar_tx, RenesasGbEthState),
        VMSTATE_UINT32(cdar_rx, RenesasGbEthState),
        VMSTATE_UINT32(esr, RenesasGbEthState),
        VMSTATE_UINT32(rcr, RenesasGbEthState),
        VMSTATE_UINT32(rtc, RenesasGbEthState),
        VMSTATE_UINT32(tgc, RenesasGbEthState),
        VMSTATE_UINT32(tccr, RenesasGbEthState),
        VMSTATE_UINT32(dic, RenesasGbEthState),
        VMSTATE_UINT32(dis, RenesasGbEthState),
        VMSTATE_UINT32(eic, RenesasGbEthState),
        VMSTATE_UINT32(eis, RenesasGbEthState),
        VMSTATE_UINT32(ric0, RenesasGbEthState),
        VMSTATE_UINT32(ris0, RenesasGbEthState),
        VMSTATE_UINT32(ric1, RenesasGbEthState),
        VMSTATE_UINT32(ric2, RenesasGbEthState),
        VMSTATE_UINT32(ris2, RenesasGbEthState),
        VMSTATE_UINT32(tic, RenesasGbEthState),
        VMSTATE_UINT32(tis, RenesasGbEthState),
        VMSTATE_UINT32(cie, RenesasGbEthState),
        VMSTATE_UINT32(ric3, RenesasGbEthState),
        VMSTATE_UINT32(ris3, RenesasGbEthState),
        VMSTATE_UINT32(ecmr, RenesasGbEthState),
        VMSTATE_UINT32(rflr, RenesasGbEthState),
        VMSTATE_UINT32(ecsr, RenesasGbEthState),
        VMSTATE_UINT32(ecsipr, RenesasGbEthState),
        VMSTATE_UINT32(pir, RenesasGbEthState),
        VMSTATE_UINT32(pipr, RenesasGbEthState),
        VMSTATE_UINT32(cxr31, RenesasGbEthState),
        VMSTATE_UINT32(cxr35, RenesasGbEthState),
        VMSTATE_UINT32(cxr71, RenesasGbEthState),
        VMSTATE_UINT32(cxr81, RenesasGbEthState),
        VMSTATE_UINT32(gecmr, RenesasGbEthState),
        VMSTATE_UINT32(cxr2g, RenesasGbEthState),
        VMSTATE_UINT32(mahr, RenesasGbEthState),
        VMSTATE_UINT32(malr, RenesasGbEthState),
        VMSTATE_UINT32(mdiomod, RenesasGbEthState),
        VMSTATE_UINT32(mc_count, RenesasGbEthState),
        VMSTATE_UINT32(csr0, RenesasGbEthState),
        VMSTATE_UINT32(csr1, RenesasGbEthState),
        VMSTATE_UINT32(csr2, RenesasGbEthState),
        VMSTATE_UINT8(mdio_state, RenesasGbEthState),
        VMSTATE_UINT8(mdio_bits, RenesasGbEthState),
        VMSTATE_UINT16(mdio_shift, RenesasGbEthState),
        VMSTATE_UINT16(mdio_data, RenesasGbEthState),
        VMSTATE_UINT8(mdio_reg, RenesasGbEthState),
        VMSTATE_BOOL(mdio_respond, RenesasGbEthState),
        VMSTATE_BOOL(mdio_out, RenesasGbEthState),
        VMSTATE_UINT8_ARRAY(tx_frame, RenesasGbEthState,
                            RENESAS_GBETH_TX_FRAME_MAX),
        VMSTATE_UINT32(tx_len, RenesasGbEthState),
        VMSTATE_BOOL(tx_in_frame, RenesasGbEthState),
        VMSTATE_BOOL(tx_oversize, RenesasGbEthState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property gbeth_properties[] = {
    DEFINE_NIC_PROPERTIES(RenesasGbEthState, conf),
    DEFINE_PROP_LINK("dma-memory", RenesasGbEthState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("phy", RenesasGbEthState, phy, TYPE_KSZ9131_PHY,
                     KSZ9131PhyState *),
    DEFINE_PROP_UINT8("phy-addr", RenesasGbEthState, phy_addr, 7),
};

static void gbeth_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Renesas RZ/G2L family Gigabit Ethernet interface";
    dc->realize = gbeth_realize;
    dc->unrealize = gbeth_unrealize;
    dc->vmsd = &vmstate_gbeth;
    device_class_set_props(dc, gbeth_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    rc->phases.hold = gbeth_reset_hold;
}

static const TypeInfo gbeth_info = {
    .name = TYPE_RENESAS_GBETH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasGbEthState),
    .instance_init = gbeth_init,
    .class_init = gbeth_class_init,
};

static void gbeth_register_types(void)
{
    type_register_static(&gbeth_info);
}

type_init(gbeth_register_types)
