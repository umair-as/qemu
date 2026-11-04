/*
 * QTest testcase for the SPI Multi I/O Bus Controller of the Renesas
 * RZ/V2L and the module's MT25QU512A flash
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Expected values come from the RZ/V2L Group User's Manual: Hardware
 * (R01UH0936EJ0150), section 25, from the MT25QU512A's JEDEC ID and
 * the SFDP tables read from the part on an RZ/V2L SMARC module, and from
 * the flash image the test writes, never from the model. Without a
 * kernel the machine starts at the manual reset values: the CPG stops
 * the controller's clocks and holds its reset (CPG_CLKON_SPI_MULTI,
 * CPG_RST_SPI reset value 0, section 7.2.4) until the test releases
 * them. The flash is backed by a temporary 64 MiB image.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define RPC_BASE            0x10060000
#define RPC_BUS             0x20000000
#define CPG_BASE            0x11010000
#define CPG_CLKON_SPI_MULTI 0x550
#define CPG_RST_SPI         0x850

/* Table 25.2 */
#define CMNCR               0x00
#define SSLDR               0x04
#define DRCR                0x0c
#define DRCMR               0x10
#define DREAR               0x14
#define DROPR               0x18
#define DRENR               0x1c
#define SMCR                0x20
#define SMCMR               0x24
#define SMADR               0x28
#define SMOPR               0x2c
#define SMENR               0x30
#define SMRDR0              0x38
#define SMRDR1              0x3c
#define SMWDR0              0x40
#define SMWDR1              0x44
#define CMNSR               0x48
#define DRDMCR              0x58
#define DRDRENR             0x5c
#define SMDMCR              0x60
#define SMDRENR             0x64
#define PHYADJ1             0x70
#define PHYADJ2             0x74
#define PHYCNT              0x7c
#define PHYOFFSET1          0x80
#define PHYOFFSET2          0x84
#define PHYINT              0x88

/* Fields (section 25.4) */
#define CMNCR_MD            (1u << 31)
#define CMNCR_BSZ           (3u << 0)
#define DRCR_SSLN           (1u << 24)
#define DRCR_RBURST(n)      (((n) - 1) << 16)
#define DRCR_RCF            (1u << 9)
#define DRCR_RBE            (1u << 8)
#define DRCR_SSLE           (1u << 0)
#define CMD(c)              ((c) << 16)
#define SMCR_SSLKP          (1u << 8)
#define SMCR_SPIRE          (1u << 2)
#define SMCR_SPIWE          (1u << 1)
#define SMCR_SPIE           (1u << 0)
#define EN_ADB(v)           ((v) << 24)
#define EN_DB(v)            ((v) << 16)
#define EN_DME              (1u << 15)
#define EN_CDE              (1u << 14)
#define EN_ADE(v)           ((v) << 8)
#define EN_SPIDE(v)         ((v) << 0)
#define CMNSR_SSLF          (1u << 1)
#define CMNSR_TEND          (1u << 0)
#define DREAR_EAV(v)        ((v) << 16)
#define DREAR_EAC(v)        ((v) << 0)
/* PHYCNT with PHYMEM = 00 (serial flash, SDR) and its fixed 1 bits */
#define PHYCNT_SDR          0x00000260

/* Flash commands (MT25QU512A) */
#define FL_WREN             0x06
#define FL_PP               0x02
#define FL_READ             0x03
#define FL_READ4            0x13
#define FL_FAST_READ        0x0b
#define FL_QIOR4            0xec
#define FL_RDID             0x9f
#define FL_RDSFDP           0x5a

#define FLASH_SIZE          (64 * 1024 * 1024)
/* Image layout: erased page at 0, patterns at PAT1 and PAT2 */
#define PAT1                0x00100000
#define PAT2                0x02000000
#define PAT_LEN             0x1000
/* Non-repeating data for the boot firmware's read path, below 16 MiB */
#define HASH_BASE           0x00a00000
#define HASH_LEN            0x3000

static uint8_t pat(uint32_t a)
{
    return (a * 37 + (a >> 9) + (a >> 17) * 11) & 0xff;
}

static uint32_t pat32(uint32_t a)
{
    return pat(a) | pat(a + 1) << 8 | pat(a + 2) << 16 |
           (uint32_t)pat(a + 3) << 24;
}

/* A byte per address from a 32-bit mix: no period within HASH_LEN */
static uint8_t hash(uint32_t a)
{
    a *= 0x9e3779b1;
    a ^= a >> 15;
    a *= 0x85ebca77;
    a ^= a >> 13;
    return a >> 24;
}

static char *make_image(void)
{
    char *path;
    uint8_t buf[PAT_LEN];
    int fd = g_file_open_tmp("rzv2l-rpc-XXXXXX.img", &path, NULL);
    int i;

    g_assert(fd >= 0);
    g_assert(ftruncate(fd, FLASH_SIZE) == 0);
    memset(buf, 0xff, sizeof(buf));
    g_assert(pwrite(fd, buf, sizeof(buf), 0) == sizeof(buf));
    for (i = 0; i < PAT_LEN; i++) {
        buf[i] = pat(PAT1 + i);
    }
    g_assert(pwrite(fd, buf, sizeof(buf), PAT1) == sizeof(buf));
    for (i = 0; i < PAT_LEN; i++) {
        buf[i] = pat(PAT2 + i);
    }
    g_assert(pwrite(fd, buf, sizeof(buf), PAT2) == sizeof(buf));
    for (i = 0; i < HASH_LEN; i++) {
        uint8_t b = hash(HASH_BASE + i);

        g_assert(pwrite(fd, &b, 1, HASH_BASE + i) == 1);
    }
    close(fd);
    return path;
}

static uint32_t rd(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, RPC_BASE + reg);
}

static void wr(QTestState *qts, uint32_t reg, uint32_t val)
{
    qtest_writel(qts, RPC_BASE + reg, val);
}

/* Supply both clocks and release the reset (CPG write-enable bits) */
static void cpg_on(QTestState *qts)
{
    qtest_writel(qts, CPG_BASE + CPG_CLKON_SPI_MULTI, 0x00030003);
    qtest_writel(qts, CPG_BASE + CPG_RST_SPI, 0x00010001);
}

static QTestState *start(const char *img)
{
    QTestState *qts = qtest_initf("-machine rzv2l-smarc "
                                  "-drive if=mtd,format=raw,file=%s", img);

    cpg_on(qts);
    /*
     * One serial flash in SDR: the reset values select two flash
     * memories (CMNCR.BSZ = 01), HyperFlash (PHYCNT.PHYMEM = 11) and DDR
     */
    wr(qts, CMNCR, rd(qts, CMNCR) & ~CMNCR_BSZ);
    wr(qts, PHYCNT, PHYCNT_SDR);
    wr(qts, DRDRENR, 0);
    return qts;
}

/* One manual mode transfer, checking that it ended */
static void manual(QTestState *qts, uint32_t cmd, uint32_t addr,
                   uint32_t smenr, uint32_t smcr)
{
    wr(qts, SMCMR, CMD(cmd));
    wr(qts, SMADR, addr);
    wr(qts, SMENR, smenr);
    wr(qts, SMCR, smcr | SMCR_SPIE);
    g_assert_cmphex(rd(qts, CMNSR) & CMNSR_TEND, ==, CMNSR_TEND);
}

static void wren(QTestState *qts)
{
    manual(qts, FL_WREN, 0, EN_CDE, 0);
}

/* External address space read mode: command, 24-bit address */
static void ext_mode(QTestState *qts, uint32_t cmd, uint32_t drcr)
{
    wr(qts, CMNCR, rd(qts, CMNCR) & ~CMNCR_MD);
    wr(qts, DRCMR, CMD(cmd));
    wr(qts, DRENR, EN_CDE | EN_ADE(0x7));
    wr(qts, DRCR, drcr);
}

/* Table 25.2 reset values, once the clocks run and the reset is off */
static void test_reset_values(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    cpg_on(qts);
    g_assert_cmphex(rd(qts, CMNCR), ==, 0x01557301);
    g_assert_cmphex(rd(qts, SSLDR), ==, 0x00000000);
    g_assert_cmphex(rd(qts, DRCR), ==, 0x001f0100);
    g_assert_cmphex(rd(qts, DRCMR), ==, 0x00a00000);
    g_assert_cmphex(rd(qts, DREAR), ==, 0x00000000);
    g_assert_cmphex(rd(qts, DROPR), ==, 0x00000000);
    g_assert_cmphex(rd(qts, DRENR), ==, 0xa222d400);
    g_assert_cmphex(rd(qts, SMCR), ==, 0x00000000);
    g_assert_cmphex(rd(qts, SMCMR), ==, 0x00000000);
    g_assert_cmphex(rd(qts, SMADR), ==, 0x00000000);
    g_assert_cmphex(rd(qts, SMOPR), ==, 0x00000000);
    g_assert_cmphex(rd(qts, SMENR), ==, 0x00004000);
    g_assert_cmphex(rd(qts, SMWDR0), ==, 0x00000000);
    g_assert_cmphex(rd(qts, SMWDR1), ==, 0x00000000);
    g_assert_cmphex(rd(qts, CMNSR), ==, 0x00000001);
    g_assert_cmphex(rd(qts, DRDMCR), ==, 0x0000000b);
    g_assert_cmphex(rd(qts, DRDRENR), ==, 0x00005101);
    g_assert_cmphex(rd(qts, SMDMCR), ==, 0x00000000);
    g_assert_cmphex(rd(qts, SMDRENR), ==, 0x00000000);
    g_assert_cmphex(rd(qts, PHYADJ1), ==, 0x00000000);
    g_assert_cmphex(rd(qts, PHYADJ2), ==, 0x00000000);
    g_assert_cmphex(rd(qts, PHYCNT), ==, 0x00000263);
    g_assert_cmphex(rd(qts, PHYOFFSET1), ==, 0x21511144);
    g_assert_cmphex(rd(qts, PHYOFFSET2), ==, 0x00000431);
    g_assert_cmphex(rd(qts, PHYINT), ==, 0x07070002);
    qtest_quit(qts);
}

/*
 * Writable and read-only bits (the R/W rows of section 25.4): all ones,
 * then all zeros. Write-only bits (DRCR.SSLN, RCF, SMCR.SPIE,
 * PHYCNT.CAL) read 0; reserved bits read their fixed value.
 */
static void test_rw_bits(void)
{
    static const struct {
        uint32_t reg, ones, zeros;
    } t[] = {
        { CMNCR,      0x81fff303, 0x01000000 },
        { SSLDR,      0x00070707, 0x00000000 },
        { DRCR,       0x001f0101, 0x00000000 },
        { DRCMR,      0x00ff00ff, 0x00000000 },
        { DREAR,      0x00ff0007, 0x00000000 },
        { DROPR,      0xffffffff, 0x00000000 },
        { DRENR,      0xf333dff0, 0x00000000 },
        { SMCMR,      0x00ff00ff, 0x00000000 },
        { SMADR,      0xffffffff, 0x00000000 },
        { SMOPR,      0xffffffff, 0x00000000 },
        { SMENR,      0xf333dfff, 0x00000000 },
        { DRDMCR,     0x0000001f, 0x00000000 },
        { DRDRENR,    0x00007111, 0x00000000 },
        { SMDMCR,     0x0000001f, 0x00000000 },
        { SMDRENR,    0x00007111, 0x00000000 },
        { PHYADJ1,    0xffffffff, 0x00000000 },
        { PHYADJ2,    0xffffffff, 0x00000000 },
        { PHYCNT,     0x40f70277, 0x00000260 },
        { PHYOFFSET1, 0x31511144, 0x01511144 },
        { PHYOFFSET2, 0x00000731, 0x00000031 },
        { PHYINT,     0x07070006, 0x00070000 },
    };
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    int i;

    cpg_on(qts);
    for (i = 0; i < ARRAY_SIZE(t); i++) {
        wr(qts, t[i].reg, 0xffffffff);
        g_assert_cmphex(rd(qts, t[i].reg), ==, t[i].ones);
        wr(qts, t[i].reg, 0);
        g_assert_cmphex(rd(qts, t[i].reg), ==, t[i].zeros);
    }
    /* SMCR without SPIE: SSLKP, SPIRE, SPIWE */
    wr(qts, SMCR, 0xfffffffe);
    g_assert_cmphex(rd(qts, SMCR), ==, 0x00000106);
    /* CMNSR is read-only */
    wr(qts, CMNSR, 0);
    g_assert_cmphex(rd(qts, CMNSR), ==, CMNSR_TEND);
    qtest_quit(qts);
}

/*
 * Manual mode reads of the JEDEC ID (20 BB 20 10 44 00): 4, 2 and 1
 * bytes. Data byte 0 is at the register's address 0, bits 7:0
 * (SMENR.SPIDE, Figure 25.32), read at the transfer size or as a word.
 */
static void test_manual_read(void)
{
    char *img = make_image();
    QTestState *qts = start(img);

    wr(qts, CMNCR, rd(qts, CMNCR) | CMNCR_MD);
    manual(qts, FL_RDID, 0, EN_CDE | EN_SPIDE(0xf), SMCR_SPIRE);
    g_assert_cmphex(rd(qts, SMRDR0), ==, 0x1020bb20);
    g_assert_cmphex(rd(qts, CMNSR), ==, CMNSR_TEND);

    manual(qts, FL_RDID, 0, EN_CDE | EN_SPIDE(0xc), SMCR_SPIRE);
    g_assert_cmphex(qtest_readw(qts, RPC_BASE + SMRDR0), ==, 0xbb20);
    g_assert_cmphex(rd(qts, SMRDR0) & 0xffff, ==, 0xbb20);

    manual(qts, FL_RDID, 0, EN_CDE | EN_SPIDE(0x8), SMCR_SPIRE);
    g_assert_cmphex(qtest_readb(qts, RPC_BASE + SMRDR0), ==, 0x20);

    /*
     * SFDP: 3-byte address, 8 dummy cycles (SMDMCR = 7); the signature,
     * then the first dword of the basic table at 0x30 as the board's
     * part returns it
     */
    wr(qts, SMDMCR, 7);
    manual(qts, FL_RDSFDP, 0, EN_CDE | EN_ADE(0x7) | EN_DME | EN_SPIDE(0xf),
           SMCR_SPIRE);
    g_assert_cmphex(rd(qts, SMRDR0), ==, 0x50444653);
    manual(qts, FL_RDSFDP, 0x30,
           EN_CDE | EN_ADE(0x7) | EN_DME | EN_SPIDE(0xf), SMCR_SPIRE);
    g_assert_cmphex(rd(qts, SMRDR0), ==, 0xfffb20e5);

    /* A read with an address: READ at PAT1 */
    manual(qts, FL_READ, PAT1, EN_CDE | EN_ADE(0x7) | EN_SPIDE(0xf),
           SMCR_SPIRE);
    g_assert_cmphex(rd(qts, SMRDR0), ==, pat32(PAT1));
    qtest_quit(qts);
    unlink(img);
    g_free(img);
}

/*
 * Manual mode page program of 10 bytes in five transfers, QSPI0_SSL held
 * by SSLKP between them: 4 bytes as a 32-bit word; 2 bytes as a 32-bit
 * word with the data in the top bytes, then at their own size; 1 byte at
 * its own size, then as a 32-bit word with the data in the top byte. The
 * flash then holds the bytes in order. The manual gives only the
 * transfer-size writes; the 32-bit ones pin the model's rule for them
 * (the data in the top bytes, as the SoC's firmware writes it).
 */
static void test_manual_write(void)
{
    char *img = make_image();
    QTestState *qts = start(img);

    wr(qts, CMNCR, rd(qts, CMNCR) | CMNCR_MD);
    wren(qts);
    g_assert_cmphex(rd(qts, CMNSR), ==, CMNSR_TEND);

    wr(qts, SMWDR0, 0x44332211);
    manual(qts, FL_PP, 0x10, EN_CDE | EN_ADE(0x7) | EN_SPIDE(0xf),
           SMCR_SPIWE | SMCR_SSLKP);
    g_assert_cmphex(rd(qts, CMNSR), ==, CMNSR_SSLF | CMNSR_TEND);

    wr(qts, SMWDR0, 0x66550000);
    manual(qts, 0, 0, EN_SPIDE(0xc), SMCR_SPIWE | SMCR_SSLKP);
    g_assert_cmphex(rd(qts, CMNSR), ==, CMNSR_SSLF | CMNSR_TEND);

    qtest_writew(qts, RPC_BASE + SMWDR0, 0x8877);
    g_assert_cmphex(qtest_readw(qts, RPC_BASE + SMWDR0), ==, 0x8877);
    manual(qts, 0, 0, EN_SPIDE(0xc), SMCR_SPIWE | SMCR_SSLKP);

    qtest_writeb(qts, RPC_BASE + SMWDR0, 0x99);
    manual(qts, 0, 0, EN_SPIDE(0x8), SMCR_SPIWE | SMCR_SSLKP);

    wr(qts, SMWDR0, 0xaa000000);
    manual(qts, 0, 0, EN_SPIDE(0x8), SMCR_SPIWE);
    g_assert_cmphex(rd(qts, CMNSR), ==, CMNSR_TEND);

    ext_mode(qts, FL_READ, 0);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + 0x10), ==, 0x44332211);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + 0x14), ==, 0x88776655);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + 0x18), ==, 0xffffaa99);
    qtest_quit(qts);
    unlink(img);
    g_free(img);
}

/*
 * External address space read mode, normal read (RBE = 0): each access
 * reads its own size at its address; a 32-bit address takes bits 32:25
 * from DREAR.EAV, a fast read sends its dummy cycles
 */
static void test_normal_read(void)
{
    char *img = make_image();
    QTestState *qts = start(img);

    ext_mode(qts, FL_READ, 0);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + PAT1 + 0x20), ==,
                    pat32(PAT1 + 0x20));
    /* QSPI0_SSL is negated after each normal read (section 25.5.10) */
    g_assert_cmphex(rd(qts, CMNSR), ==, CMNSR_TEND);
    g_assert_cmphex(qtest_readb(qts, RPC_BUS + PAT1 + 0x123), ==,
                    pat(PAT1 + 0x123));
    g_assert_cmphex(qtest_readq(qts, RPC_BUS + PAT1 + 0x40), ==,
                    pat32(PAT1 + 0x40) |
                    (uint64_t)pat32(PAT1 + 0x44) << 32);

    /* Fast read: 8 dummy cycles on 1 line (DRDMCR = 7) */
    wr(qts, DRCMR, CMD(FL_FAST_READ));
    wr(qts, DRENR, EN_CDE | EN_ADE(0x7) | EN_DME);
    wr(qts, DRDMCR, 7);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + PAT1 + 0x80), ==,
                    pat32(PAT1 + 0x80));

    /*
     * Quad I/O fast read with a 32-bit address (1-4-4, ECh), as Linux
     * reads the part: address and data on 4 lines, 10 dummy cycles
     * (DRDMCR = 9), the default the part's SFDP gives
     */
    wr(qts, DRCMR, CMD(FL_QIOR4));
    wr(qts, DRENR, EN_CDE | EN_ADE(0xf) | EN_DME | EN_ADB(2) | EN_DB(2));
    wr(qts, DREAR, DREAR_EAC(1));
    wr(qts, DRDMCR, 9);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + PAT1 + 0x100), ==,
                    pat32(PAT1 + 0x100));
    wr(qts, DRENR, EN_CDE | EN_ADE(0x7));

    /* 32-bit address: bus space bits 24:0, flash bit 25 from EAV bit 0 */
    wr(qts, DRCMR, CMD(FL_READ4));
    wr(qts, DRENR, EN_CDE | EN_ADE(0xf));
    wr(qts, DREAR, DREAR_EAV(1) | DREAR_EAC(0));
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + 0x10), ==, pat32(PAT2 + 0x10));
    /* EAC = 1: bus space bits 25:0, EAV bit 0 ignored */
    wr(qts, DREAR, DREAR_EAV(0) | DREAR_EAC(1));
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + PAT2 + 0x30), ==,
                    pat32(PAT2 + 0x30));
    qtest_quit(qts);
    unlink(img);
    g_free(img);
}

/*
 * Burst read through the read cache (RBE = 1, section 25.5.6): a hit
 * reads no flash, so a changed flash still reads its old data until
 * DRCR.RCF clears the cache; a burst fills RBURST lines of 8 bytes
 */
static void test_read_cache(void)
{
    char *img = make_image();
    QTestState *qts = start(img);

    ext_mode(qts, FL_READ, DRCR_RBE | DRCR_RBURST(2));
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + 0x00), ==, 0xffffffff);

    /* Program 16 bytes at 0 with 0x00 */
    wr(qts, CMNCR, rd(qts, CMNCR) | CMNCR_MD);
    wren(qts);
    wr(qts, SMWDR0, 0);
    manual(qts, FL_PP, 0, EN_CDE | EN_ADE(0x7) | EN_SPIDE(0xf),
           SMCR_SPIWE | SMCR_SSLKP);
    manual(qts, 0, 0, EN_SPIDE(0xf), SMCR_SPIWE | SMCR_SSLKP);
    manual(qts, 0, 0, EN_SPIDE(0xf), SMCR_SPIWE | SMCR_SSLKP);
    manual(qts, 0, 0, EN_SPIDE(0xf), SMCR_SPIWE);
    wr(qts, CMNCR, rd(qts, CMNCR) & ~CMNCR_MD);

    /* Bytes 0-15 were one burst: cache hits */
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + 0x04), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + 0x0c), ==, 0xffffffff);
    /* Flush: the flash's new data */
    wr(qts, DRCR, DRCR_RBE | DRCR_RBURST(2) | DRCR_RCF);
    g_assert_cmphex(rd(qts, DRCR), ==, DRCR_RBE | DRCR_RBURST(2));
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + 0x04), ==, 0x00000000);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + 0x0c), ==, 0x00000000);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + 0x10), ==, 0xffffffff);
    qtest_quit(qts);
    unlink(img);
    g_free(img);
}

/*
 * The read path of TF-A's BL2 (plat/renesas/rz/common/drivers/spi_multi/
 * spi_multi.c, MT25QU512ABB/spi_multi_reg_values.h), with its register
 * values: quad I/O fast read with a 24-bit address (1-4-4, EBh), 10
 * dummy cycles (DRDMCR = 9), the read cache on with 32-line bursts and
 * a flush (DRCR 0x001f0300). It reads its FIP this way. 12 KiB of
 * non-repeating data from an unaligned address, 48 bursts, compared
 * byte by byte with the image the test wrote; then the burst length,
 * through a flash write the cache does not see.
 */
static void test_bl2_read(void)
{
    char *img = make_image();
    QTestState *qts = start(img);
    uint32_t from = HASH_BASE + 0x13, len = HASH_LEN - 0x13 - 0x5;
    g_autofree uint8_t *buf = g_malloc(len);
    uint32_t i;

    wr(qts, PHYCNT, 0x00000260);
    wr(qts, PHYOFFSET1, 0x31511144);
    wr(qts, PHYOFFSET2, 0x00000431);
    wr(qts, CMNCR, 0x0155a200);
    wr(qts, SSLDR, 0x00030303);
    wr(qts, DRCR, 0x001f0300);
    rd(qts, DRCR);
    wr(qts, DRCMR, CMD(0xeb));
    wr(qts, DREAR, 0);
    wr(qts, DRENR, 0x0202c700);
    wr(qts, DRDMCR, 9);
    wr(qts, DRDRENR, 0);
    wr(qts, PHYCNT, 0x00030260);

    qtest_memread(qts, RPC_BUS + from, buf, len);
    for (i = 0; i < len; i++) {
        if (buf[i] != hash(from + i)) {
            g_test_message("flash 0x%x: read 0x%02x, wrote 0x%02x",
                           from + i, buf[i], hash(from + i));
        }
        g_assert_cmphex(buf[i], ==, hash(from + i));
    }
    /*
     * A burst is 32 lines: program 4 bytes in its last line and 4 bytes
     * after it; the last line is still a cache hit with the old data,
     * the line after it a miss with the new
     */
    g_assert_cmphex(qtest_readb(qts, RPC_BUS + HASH_BASE + 0x1000), ==,
                    hash(HASH_BASE + 0x1000));
    wr(qts, CMNCR, 0x0155a200 | CMNCR_MD);
    wr(qts, SMWDR0, 0);
    wren(qts);
    manual(qts, FL_PP, HASH_BASE + 0x10f8,
           EN_CDE | EN_ADE(0x7) | EN_SPIDE(0xf), SMCR_SPIWE);
    wren(qts);
    manual(qts, FL_PP, HASH_BASE + 0x1100,
           EN_CDE | EN_ADE(0x7) | EN_SPIDE(0xf), SMCR_SPIWE);
    wr(qts, CMNCR, 0x0155a200);
    g_assert_cmphex(qtest_readb(qts, RPC_BUS + HASH_BASE + 0x10f8), ==,
                    hash(HASH_BASE + 0x10f8));
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + HASH_BASE + 0x1100), ==, 0);

    /* Word reads across a burst boundary, after the bursts above */
    for (i = 0xf8; i < 0x110; i += 4) {
        g_assert_cmphex(qtest_readl(qts, RPC_BUS + HASH_BASE + i), ==,
                        hash(HASH_BASE + i) |
                        hash(HASH_BASE + i + 1) << 8 |
                        hash(HASH_BASE + i + 2) << 16 |
                        (uint32_t)hash(HASH_BASE + i + 3) << 24);
    }
    qtest_quit(qts);
    unlink(img);
    g_free(img);
}

/*
 * DRCR.SSLE = 1: QSPI0_SSL stays asserted after a burst and TEND reads 0
 * until SSLN negates it (sections 25.4.3, 25.5.12); the next burst at
 * the following address continues the same flash command
 */
static void test_ssle(void)
{
    char *img = make_image();
    QTestState *qts = start(img);

    ext_mode(qts, FL_READ, DRCR_RBE | DRCR_RBURST(2) | DRCR_SSLE);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + PAT1), ==, pat32(PAT1));
    g_assert_cmphex(rd(qts, CMNSR), ==, CMNSR_SSLF);
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + PAT1 + 0x10), ==,
                    pat32(PAT1 + 0x10));
    g_assert_cmphex(qtest_readl(qts, RPC_BUS + PAT1 + 0x200), ==,
                    pat32(PAT1 + 0x200));
    g_assert_cmphex(rd(qts, CMNSR), ==, CMNSR_SSLF);
    wr(qts, DRCR, DRCR_RBE | DRCR_RBURST(2) | DRCR_SSLE | DRCR_SSLN);
    g_assert_cmphex(rd(qts, CMNSR), ==, CMNSR_TEND);
    qtest_quit(qts);
    unlink(img);
    g_free(img);
}

/*
 * The CPG gates: while CPG_RST_SPI holds the reset, writes do not take
 * and the release leaves the reset values; while CPG_CLKON_SPI_MULTI
 * stops a clock, writes do not take and the registers keep their values
 */
static void test_gates(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    cpg_on(qts);
    wr(qts, SMADR, 0x12345678);
    qtest_writel(qts, CPG_BASE + CPG_CLKON_SPI_MULTI, 0x00010000);
    wr(qts, SMADR, 0x9abcdef0);
    qtest_writel(qts, CPG_BASE + CPG_CLKON_SPI_MULTI, 0x00010001);
    g_assert_cmphex(rd(qts, SMADR), ==, 0x12345678);

    qtest_writel(qts, CPG_BASE + CPG_RST_SPI, 0x00010000);
    wr(qts, SMADR, 0x9abcdef0);
    qtest_writel(qts, CPG_BASE + CPG_RST_SPI, 0x00010001);
    g_assert_cmphex(rd(qts, SMADR), ==, 0x00000000);
    g_assert_cmphex(rd(qts, CMNCR), ==, 0x01557301);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rzv2l-rpc/reset-values", test_reset_values);
    qtest_add_func("/rzv2l-rpc/rw-bits", test_rw_bits);
    qtest_add_func("/rzv2l-rpc/manual-read", test_manual_read);
    qtest_add_func("/rzv2l-rpc/manual-write", test_manual_write);
    qtest_add_func("/rzv2l-rpc/normal-read", test_normal_read);
    qtest_add_func("/rzv2l-rpc/read-cache", test_read_cache);
    qtest_add_func("/rzv2l-rpc/bl2-read", test_bl2_read);
    qtest_add_func("/rzv2l-rpc/ssle", test_ssle);
    qtest_add_func("/rzv2l-rpc/gates", test_gates);

    return g_test_run();
}
