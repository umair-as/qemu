/*
 * QTest testcase for the Gigabit Ethernet interface of the Renesas RZ/V2L,
 * its KSZ9131 PHY on the SMARC module and the IRQ0-7 inputs of the IRQC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Expected values come from the RZ/V2L Group User's Manual: Hardware
 * (R01UH0936EJ0150), chapters 7, 8 and 30, from IEEE 802.3 clause 22
 * for the PHY registers, from the PHY identifier the board's device
 * tree gives (compatible "ethernet-phy-id0022.1640", address 7), and
 * from the frames the test sends, never from the model.
 *
 * Without a kernel the machine starts at the manual reset values:
 * CPG_RST_ETH holds both channels in reset (section 7.2.4, reset value
 * 0) until the test releases them. ETH0's NIC is one end of a socket
 * pair; the test is the link partner at the other end.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "libqtest.h"
#include <sys/socket.h>

#define ETH0                0x11c20000
#define CPG                 0x11010000
#define IRQC                0x110a0000

/* CPG (Table 7.5) */
#define CPG_RST_ETH         0x87c
#define CPG_RSTMON_ETH      0x9fc

/* IRQC (Table 8.3) */
#define ISCR                0x10
#define IITSR               0x14

/* DMAC (Table 30.4) */
#define CCC                 0x000
#define DBAT                0x004
#define DLR                 0x008
#define CSR                 0x00c
#define CDAR0               0x010
#define CDAR4               0x020
#define ESR                 0x088
#define RCR                 0x090
#define RTC                 0x0b4
#define TGC                 0x300
#define TCCR                0x304
#define EIS                 0x35c
#define RIC0                0x360
#define RIS0                0x364
#define RIC2                0x370
#define RIS2                0x374
#define TIC                 0x378
#define TIS                 0x37c
#define ISS                 0x380

/* E-MAC (Table 30.5) */
#define CXR20               0x500   /* ECMR */
#define CXR21               0x510   /* ECSR */
#define CXR22               0x518   /* ECSIPR */
#define CXR23               0x520   /* PIR */
#define CXR2B               0x528   /* PSR */
#define CXR31               0x530
#define CXR35               0x540
#define CXR24               0x5c0   /* MAHR */
#define CXR25               0x5c8   /* MALR */
#define MDIOMOD             0x790

/* TOE (Table 30.6) */
#define CSR0                0x800
#define CSR2                0x808

/* Reset values (Tables 30.4, 30.5; section 30.4.1.4) */
#define CSR_RESET           0x00000001
#define DLR_RESET           0x003fffff
#define RCR_RESET           0x60000000
#define RTC_RESET           0x7ffc7ffc
#define TGC_RESET           0x00222200
#define CXR35_RESET         0xffff0000
#define MDIOMOD_RESET       0x800000ff
#define RSTMON_ETH_RESET    0x00000003  /* section 7.2.4.171 */

/* CSR.OPS = 1 << CCC.OPC (section 30.4.1.4) */
#define OPC_CONFIG          1
#define OPC_OPERATION       2

#define TSRQ0               (1u << 0)
#define FRF0                (1u << 0)   /* RIS0 */
#define QFF0                (1u << 0)   /* RIS2 */
#define FTF0                (1u << 0)   /* TIS */
#define QEF                 (1u << 2)   /* EIS */
#define QFS                 (1u << 16)  /* EIS */
#define ISS_FRM             (1u << 0)
#define ISS_FTM             (1u << 2)
#define ISS_EM              (1u << 6)
#define ISS_MM              (1u << 7)
#define ECMR_TPE            (1u << 5)
#define ECMR_RPE            (1u << 6)
#define ECMR_RCPT           (1u << 25)
#define ECSR_LINKI          (1u << 2)
#define LINKIM              (1u << 2)
#define CSR0_RPE            (1u << 5)
/* CSR2: RIP4, RTCP4, RUDP4, RICMP4 (section 30.4.3.3) */
#define CSR2_IPV4_ALL       0x00000071

/* PIR bits (section 30.4.2.5) */
#define MDC                 (1u << 0)
#define MM                  (1u << 1)
#define MDO                 (1u << 2)
#define MDI                 (1u << 3)

/* Descriptor types (Table 30.7) */
#define DT_LEMPTY           0x2u
#define DT_EEMPTY           0x3u
#define DT_FMID             0x4u
#define DT_FSTART           0x5u
#define DT_FEND             0x6u
#define DT_FSINGLE          0x7u
#define DT_LINKFIX          0x9u
#define DT_FEMPTY           0xcu
#define MSC_CEEF            0x40    /* checksum error (Table 30.13) */
#define MSC_UNSUPPORTED     0x20
#define MSC_MC              0x80

/* ESR.ET (section 30.4.1.6) */
#define ET_RD_DATA          4
#define ET_INTERPRET        2

/* The PHY: IEEE 802.3 22.2.4, and the board's device tree */
#define PHY_ADDR            7
#define MII_BMCR            0
#define MII_BMSR            1
#define MII_PHYID1          2
#define MII_PHYID2          3
#define MII_ANAR            4
#define MII_ANLPAR          5
#define ANAR_10_ONLY        0x0061
#define ANAR_100FULL        (1u << 8)
#define BMSR_LINK           (1u << 2)
#define BMSR_ANEG_COMPLETE  (1u << 5)
#define BMCR_ANRESTART      (1u << 9)
#define BMCR_RESET          (1u << 15)
#define PHYID1_KSZ9131      0x0022
#define PHYID2_KSZ9131      0x1640  /* revision bits 3:0 masked */
/* KSZ9131 interrupt control/status: link up/down enable 8/10, status 0/2 */
#define KSZ_INT_CS          0x1b
#define KSZ_INT_LINK_DOWN_EN 0x0400
#define KSZ_INT_LINK_UP_EN  0x0100
#define KSZ_INT_LINK_DOWN   0x0004
#define PHY_IRQ_ETH0        2       /* INT_N of the ETH0 PHY is IRQ2 */

/* Guest memory used by the tests */
#define DBAT_TABLE          0x48000000
#define TX_RING             0x48001000
#define RX_RING             0x48002000
#define TX_BUF              0x48010000
#define RX_BUF              0x48020000
#define UNMAPPED            0x30000000

static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

static uint32_t eth_read(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, ETH0 + reg);
}

static void eth_write(QTestState *qts, uint32_t reg, uint32_t val)
{
    qtest_writel(qts, ETH0 + reg, val);
}

static void desc_write(QTestState *qts, uint64_t addr, uint32_t dt,
                       uint32_t ds, uint32_t dptr)
{
    qtest_writel(qts, addr, dt << 28 | ds);
    qtest_writel(qts, addr + 4, dptr);
}

static QTestState *gbeth_init(int *fd)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-machine rzv2l-smarc "
                      "-netdev socket,id=net0,fd=%d "
                      "-net nic,netdev=net0,macaddr=02:00:00:00:00:01", sv[1]);
    close(sv[1]);
    *fd = sv[0];
    return qts;
}

/* Release ETH0 from the CPG reset (CPG_RST_ETH bit 0, write enable 16) */
static void gbeth_release(QTestState *qts)
{
    qtest_writel(qts, CPG + CPG_RST_ETH, 0x00010001);
}

/* The mdio-bitbang sequence of Linux, on CXR23 */
static void pir_modify(QTestState *qts, uint32_t mask, bool set)
{
    uint32_t v = eth_read(qts, CXR23);

    eth_write(qts, CXR23, set ? v | mask : v & ~mask);
}

static void mdio_send_bit(QTestState *qts, int bit)
{
    pir_modify(qts, MDO, bit);
    pir_modify(qts, MDC, true);
    pir_modify(qts, MDC, false);
}

static int mdio_get_bit(QTestState *qts)
{
    pir_modify(qts, MDC, true);
    pir_modify(qts, MDC, false);
    return !!(eth_read(qts, CXR23) & MDI);
}

static void mdio_cmd(QTestState *qts, int op, int phy, int reg)
{
    int i;

    pir_modify(qts, MM, true);
    for (i = 0; i < 32; i++) {
        mdio_send_bit(qts, 1);
    }
    mdio_send_bit(qts, 0);
    mdio_send_bit(qts, 1);
    mdio_send_bit(qts, op >> 1);
    mdio_send_bit(qts, op & 1);
    for (i = 4; i >= 0; i--) {
        mdio_send_bit(qts, phy >> i & 1);
    }
    for (i = 4; i >= 0; i--) {
        mdio_send_bit(qts, reg >> i & 1);
    }
}

static int mdio_read(QTestState *qts, int phy, int reg)
{
    int i, val = 0;

    mdio_cmd(qts, 2, phy, reg);
    pir_modify(qts, MM, false);
    if (mdio_get_bit(qts) != 0) {
        for (i = 0; i < 32; i++) {
            mdio_get_bit(qts);
        }
        return 0xffff;
    }
    for (i = 0; i < 16; i++) {
        val = val << 1 | mdio_get_bit(qts);
    }
    mdio_get_bit(qts);
    return val;
}

static void mdio_write(QTestState *qts, int phy, int reg, uint16_t val)
{
    int i;

    mdio_cmd(qts, 1, phy, reg);
    mdio_send_bit(qts, 1);
    mdio_send_bit(qts, 0);
    for (i = 15; i >= 0; i--) {
        mdio_send_bit(qts, val >> i & 1);
    }
    pir_modify(qts, MM, false);
    mdio_get_bit(qts);
}

static void set_link(QTestState *qts, bool up)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'set_link', 'arguments': "
                           "{ 'name': 'net0', 'up': %i } }", up);

    g_assert(qdict_haskey(rsp, "return"));
    qobject_unref(rsp);
}

static void set_opc(QTestState *qts, uint32_t opc)
{
    eth_write(qts, CCC, (eth_read(qts, CCC) & ~3u) | opc);
    g_assert_cmphex(eth_read(qts, CSR), ==, 1u << opc);
}

/*
 * Bring ETH0 up the way the Linux driver does: CONFIG, the table, the
 * rings, OPERATION, then the E-MAC with TPE and RPE and the address.
 */
static void gbeth_start(QTestState *qts, uint32_t ecmr)
{
    gbeth_release(qts);
    set_opc(qts, OPC_CONFIG);
    eth_write(qts, DBAT, DBAT_TABLE);
    desc_write(qts, DBAT_TABLE + 0x00, DT_LINKFIX, 0, TX_RING);
    desc_write(qts, DBAT_TABLE + 0x20, DT_LINKFIX, 0, RX_RING);
    eth_write(qts, RIC0, FRF0);
    eth_write(qts, RIC2, QFF0);
    eth_write(qts, TIC, FTF0);
    set_opc(qts, OPC_OPERATION);
    eth_write(qts, CXR24, ldl_be_p(mac));
    eth_write(qts, CXR25, lduw_be_p(mac + 4));
    eth_write(qts, CXR20, ecmr | ECMR_TPE | ECMR_RPE);
}

/* The socket netdev frames each packet with a 4-byte big-endian length */
static void sock_send(int fd, const uint8_t *buf, uint32_t len)
{
    uint32_t be = cpu_to_be32(len);

    g_assert_cmpint(write(fd, &be, 4), ==, 4);
    g_assert_cmpint(write(fd, buf, len), ==, len);
}

static uint32_t sock_recv(int fd, uint8_t *buf, uint32_t max)
{
    uint32_t be, len;

    g_assert_cmpint(recv(fd, &be, 4, MSG_WAITALL), ==, 4);
    len = be32_to_cpu(be);
    g_assert_cmpuint(len, <=, max);
    g_assert_cmpint(recv(fd, buf, len, MSG_WAITALL), ==, len);
    return len;
}

/* Let the main loop deliver a frame from the socket */
static void wait_rx(QTestState *qts, uint32_t reg, uint32_t bit)
{
    int i;

    for (i = 0; i < 1000 && !(eth_read(qts, reg) & bit); i++) {
        qtest_clock_step(qts, 10000);
        g_usleep(1000);
    }
    g_assert_cmphex(eth_read(qts, reg) & bit, ==, bit);
}

static void frame_fill(uint8_t *f, size_t len, uint8_t seed)
{
    size_t i;

    memcpy(f, mac, 6);
    memcpy(f + 6, "\x02\x00\x00\x00\x00\x02", 6);
    f[12] = 0x88;   /* local experimental EtherType 0x88b5 */
    f[13] = 0xb5;
    for (i = 14; i < len; i++) {
        f[i] = seed + i;
    }
}

static void test_reset_values(void)
{
    int fd;
    QTestState *qts = gbeth_init(&fd);

    g_assert_cmphex(qtest_readl(qts, CPG + CPG_RSTMON_ETH), ==,
                    RSTMON_ETH_RESET);
    gbeth_release(qts);
    g_assert_cmphex(qtest_readl(qts, CPG + CPG_RSTMON_ETH), ==, 0x2);
    g_assert_cmphex(eth_read(qts, CCC), ==, 0);
    g_assert_cmphex(eth_read(qts, CSR), ==, CSR_RESET);
    g_assert_cmphex(eth_read(qts, DLR), ==, DLR_RESET);
    g_assert_cmphex(eth_read(qts, RCR), ==, RCR_RESET);
    g_assert_cmphex(eth_read(qts, RTC), ==, RTC_RESET);
    g_assert_cmphex(eth_read(qts, TGC), ==, TGC_RESET);
    g_assert_cmphex(eth_read(qts, CXR35), ==, CXR35_RESET);
    g_assert_cmphex(eth_read(qts, MDIOMOD), ==, MDIOMOD_RESET);
    g_assert_cmphex(eth_read(qts, CXR24), ==, 0);
    g_assert_cmphex(eth_read(qts, CXR25), ==, 0);
    g_assert_cmphex(eth_read(qts, ISS), ==, 0);
    qtest_quit(qts);
    close(fd);
}

/* While ETHn_RST_HW_N is low the interface keeps its reset values */
static void test_cpg_reset(void)
{
    int fd;
    QTestState *qts = gbeth_init(&fd);

    eth_write(qts, CXR24, 0x12345678);
    g_assert_cmphex(eth_read(qts, CXR24), ==, 0);
    gbeth_release(qts);
    eth_write(qts, CXR24, 0x12345678);
    g_assert_cmphex(eth_read(qts, CXR24), ==, 0x12345678);
    set_opc(qts, OPC_CONFIG);
    qtest_writel(qts, CPG + CPG_RST_ETH, 0x00010000);
    g_assert_cmphex(eth_read(qts, CXR24), ==, 0);
    g_assert_cmphex(eth_read(qts, CSR), ==, CSR_RESET);
    qtest_quit(qts);
    close(fd);
}

/* The PHY at address 7 answers clause 22 frames through CXR23 */
static void test_mdio(void)
{
    int fd;
    QTestState *qts = gbeth_init(&fd);

    gbeth_release(qts);
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, MII_PHYID1), ==, PHYID1_KSZ9131);
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, MII_PHYID2) & 0xfff0, ==,
                    PHYID2_KSZ9131);
    /* No PHY answers at another address: the turnaround reads 1 */
    g_assert_cmphex(mdio_read(qts, 1, MII_PHYID1), ==, 0xffff);
    /* 10BASE-T only (22.2.4.3.1) */
    mdio_write(qts, PHY_ADDR, MII_ANAR, ANAR_10_ONLY);
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, MII_ANAR), ==, ANAR_10_ONLY);
    /* The restart bit is self-clearing (22.2.4.1.7) */
    mdio_write(qts, PHY_ADDR, MII_BMCR, 0x1000 | BMCR_ANRESTART);
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, MII_BMCR) & BMCR_ANRESTART, ==,
                    0);
    /* A cable: the link is up and negotiated */
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, MII_BMSR) &
                    (BMSR_LINK | BMSR_ANEG_COMPLETE), ==,
                    BMSR_LINK | BMSR_ANEG_COMPLETE);
    /*
     * The reset bit is self-clearing and restores the default
     * advertisement, which includes 100BASE-TX full duplex (22.2.4.1.1)
     */
    mdio_write(qts, PHY_ADDR, MII_BMCR, BMCR_RESET);
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, MII_BMCR) & BMCR_RESET, ==, 0);
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, MII_ANAR) & ANAR_100FULL, ==,
                    ANAR_100FULL);
    qtest_quit(qts);
    close(fd);
}

/*
 * Link status is latched low (22.2.4.2.13): a loss reads 0 once, even
 * when the link is back; the PHY interrupt drives IRQ2 low, which the
 * IRQC detects as a low level (IITSR = 0, section 8.6.4) and sends to
 * the GIC until the status is read.
 */
static void test_link_irq(void)
{
    int fd;
    QTestState *qts = gbeth_init(&fd);

    qtest_irq_intercept_out_named(qts, "/machine/soc/irqc", "sysbus-irq");
    gbeth_release(qts);
    mdio_read(qts, PHY_ADDR, KSZ_INT_CS);
    mdio_write(qts, PHY_ADDR, KSZ_INT_CS, KSZ_INT_LINK_DOWN_EN);
    g_assert_false(qtest_get_irq(qts, PHY_IRQ_ETH0));
    g_assert_cmphex(qtest_readl(qts, IRQC + ISCR), ==, 0);

    set_link(qts, false);
    g_assert_true(qtest_get_irq(qts, PHY_IRQ_ETH0));
    g_assert_cmphex(qtest_readl(qts, IRQC + ISCR), ==, 1u << PHY_IRQ_ETH0);
    /* A 0 written in low-level mode does not clear (section 8.6.3) */
    qtest_writel(qts, IRQC + ISCR, 0);
    g_assert_cmphex(qtest_readl(qts, IRQC + ISCR), ==, 1u << PHY_IRQ_ETH0);
    set_link(qts, true);
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, MII_BMSR) & BMSR_LINK, ==, 0);
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, MII_BMSR) & BMSR_LINK, ==,
                    BMSR_LINK);
    /* Reading the status clears it, and the pin returns high */
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, KSZ_INT_CS) & KSZ_INT_LINK_DOWN,
                    ==, KSZ_INT_LINK_DOWN);
    g_assert_false(qtest_get_irq(qts, PHY_IRQ_ETH0));
    g_assert_cmphex(qtest_readl(qts, IRQC + ISCR), ==, 0);

    /*
     * Falling-edge detection: the pin going low sets ISTAT2, which stays
     * set when the pin returns high, until a 0 is written
     */
    qtest_writel(qts, IRQC + IITSR, 1u << (2 * PHY_IRQ_ETH0));
    set_link(qts, false);
    g_assert_cmphex(qtest_readl(qts, IRQC + ISCR), ==, 1u << PHY_IRQ_ETH0);
    g_assert_cmphex(mdio_read(qts, PHY_ADDR, KSZ_INT_CS) & KSZ_INT_LINK_DOWN,
                    ==, KSZ_INT_LINK_DOWN);
    g_assert_cmphex(qtest_readl(qts, IRQC + ISCR), ==, 1u << PHY_IRQ_ETH0);
    g_assert_true(qtest_get_irq(qts, PHY_IRQ_ETH0));
    qtest_writel(qts, IRQC + ISCR, 0);
    g_assert_cmphex(qtest_readl(qts, IRQC + ISCR), ==, 0);
    g_assert_false(qtest_get_irq(qts, PHY_IRQ_ETH0));
    /* A rising edge is not detected in this mode */
    mdio_write(qts, PHY_ADDR, KSZ_INT_CS, KSZ_INT_LINK_UP_EN);
    set_link(qts, true);
    g_assert_cmphex(qtest_readl(qts, IRQC + ISCR), ==, 1u << PHY_IRQ_ETH0);
    qtest_writel(qts, IRQC + ISCR, 0);
    mdio_read(qts, PHY_ADDR, KSZ_INT_CS);
    g_assert_cmphex(qtest_readl(qts, IRQC + ISCR), ==, 0);
    qtest_quit(qts);
    close(fd);
}

/* CXR21.LINKI follows a change of the in-band link (CXR31 = 01b) */
static void test_mac_link(void)
{
    int fd;
    QTestState *qts = gbeth_init(&fd);

    qtest_irq_intercept_out_named(qts, "/machine/soc/eth0", "sysbus-irq");
    gbeth_release(qts);
    eth_write(qts, CXR31, 0x1);
    eth_write(qts, CXR21, ECSR_LINKI);
    g_assert_cmphex(eth_read(qts, CXR2B) & 1, ==, 1);
    eth_write(qts, CXR22, LINKIM);
    g_assert_false(qtest_get_irq(qts, 0));
    set_link(qts, false);
    g_assert_cmphex(eth_read(qts, CXR2B) & 1, ==, 0);
    g_assert_cmphex(eth_read(qts, CXR21) & ECSR_LINKI, ==, ECSR_LINKI);
    g_assert_cmphex(eth_read(qts, ISS) & ISS_MM, ==, ISS_MM);
    g_assert_true(qtest_get_irq(qts, 0));
    /* Write 1 to clear (section 30.4.2.3) */
    eth_write(qts, CXR21, 0);
    g_assert_cmphex(eth_read(qts, CXR21) & ECSR_LINKI, ==, ECSR_LINKI);
    eth_write(qts, CXR21, ECSR_LINKI);
    g_assert_cmphex(eth_read(qts, CXR21) & ECSR_LINKI, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
    close(fd);
}

/*
 * Transmission: a frame in an FSTART and an FEND descriptor, as the
 * Linux driver builds it, reaches the link partner; both descriptors
 * are written back FEMPTY, TSRQ0 reads 0 at the next empty descriptor,
 * TIS.FTF0 raises pif_int_n while TIC.FTE0 is set; the interrupt drops
 * when the flag is cleared by a 0 or masked.
 */
static void test_tx(void)
{
    uint8_t frame[100], got[2048];
    int fd;
    QTestState *qts = gbeth_init(&fd);

    qtest_irq_intercept_out_named(qts, "/machine/soc/eth0", "sysbus-irq");
    gbeth_start(qts, 0);
    frame_fill(frame, sizeof(frame), 0x10);
    qtest_memwrite(qts, TX_BUF, frame, sizeof(frame));
    desc_write(qts, TX_RING + 0, DT_FSTART, 4, TX_BUF);
    desc_write(qts, TX_RING + 8, DT_FEND, sizeof(frame) - 4, TX_BUF + 4);
    desc_write(qts, TX_RING + 16, DT_EEMPTY, 0, 0);
    eth_write(qts, TCCR, TSRQ0);

    g_assert_cmpuint(sock_recv(fd, got, sizeof(got)), ==, sizeof(frame));
    g_assert_cmpmem(got, sizeof(frame), frame, sizeof(frame));
    g_assert_cmphex(eth_read(qts, TCCR) & TSRQ0, ==, 0);
    g_assert_cmphex(qtest_readl(qts, TX_RING + 0), ==, DT_FEMPTY << 28 | 4);
    g_assert_cmphex(qtest_readl(qts, TX_RING + 8), ==,
                    DT_FEMPTY << 28 | (sizeof(frame) - 4));
    g_assert_cmphex(qtest_readl(qts, TX_RING + 16) >> 28, ==, DT_EEMPTY);
    g_assert_cmphex(eth_read(qts, CDAR0), ==, TX_RING + 16);
    g_assert_cmphex(eth_read(qts, TIS), ==, FTF0);
    g_assert_cmphex(eth_read(qts, ISS), ==, ISS_FTM);
    g_assert_true(qtest_get_irq(qts, 0));
    /* Masked: the line drops, the flag stays */
    eth_write(qts, TIC, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(eth_read(qts, TIS), ==, FTF0);
    eth_write(qts, TIC, FTF0);
    g_assert_true(qtest_get_irq(qts, 0));
    /* A 1 does not clear, a 0 does (section 30.4.1.38) */
    eth_write(qts, TIS, FTF0);
    g_assert_cmphex(eth_read(qts, TIS), ==, FTF0);
    eth_write(qts, TIS, ~FTF0);
    g_assert_cmphex(eth_read(qts, TIS), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    /* The next request starts at the EEMPTY where the queue stopped */
    desc_write(qts, TX_RING + 16, DT_FSINGLE, 60, TX_BUF);
    desc_write(qts, TX_RING + 24, DT_LINKFIX, 0, TX_RING);
    desc_write(qts, TX_RING + 0, DT_EEMPTY, 0, 0);
    eth_write(qts, TCCR, TSRQ0);
    g_assert_cmpuint(sock_recv(fd, got, sizeof(got)), ==, 60);
    g_assert_cmpmem(got, 60, frame, 60);
    /* LINKFIX: followed, not written back; the ring wraps to its start */
    g_assert_cmphex(qtest_readl(qts, TX_RING + 24) >> 28, ==, DT_LINKFIX);
    g_assert_cmphex(eth_read(qts, CDAR0), ==, TX_RING);
    qtest_quit(qts);
    close(fd);
}

/*
 * Reception: a frame larger than one descriptor is split FSTART, FMID,
 * FEND with each descriptor's length in DS; the last carries EI. The
 * next frame goes to the next descriptor, after the LINKFIX back to the
 * ring start. With no FEMPTY descriptor left the frame is discarded and
 * RIS2.QFF0 is set (Table 30.12); the queue retries the same descriptor.
 */
static void test_rx(void)
{
    uint8_t frame[150], got[150];
    int fd, i;
    QTestState *qts = gbeth_init(&fd);

    qtest_irq_intercept_out_named(qts, "/machine/soc/eth0", "sysbus-irq");
    for (i = 0; i < 4; i++) {
        desc_write(qts, RX_RING + 8 * i, DT_FEMPTY, 64, RX_BUF + 0x100 * i);
    }
    desc_write(qts, RX_RING + 32, DT_LINKFIX, 0, RX_RING);
    gbeth_start(qts, 0);

    frame_fill(frame, sizeof(frame), 0x20);
    sock_send(fd, frame, sizeof(frame));
    wait_rx(qts, RIS0, FRF0);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(eth_read(qts, ISS), ==, ISS_FRM);
    g_assert_cmphex(qtest_readl(qts, RX_RING + 0), ==, DT_FSTART << 28 | 64);
    g_assert_cmphex(qtest_readl(qts, RX_RING + 8), ==, DT_FMID << 28 | 64);
    g_assert_cmphex(qtest_readl(qts, RX_RING + 16), ==,
                    DT_FEND << 28 | 1u << 13 | 22);
    for (i = 0; i < 3; i++) {
        qtest_memread(qts, RX_BUF + 0x100 * i, got + 64 * i,
                      MIN(64, sizeof(frame) - 64 * i));
    }
    g_assert_cmpmem(got, sizeof(frame), frame, sizeof(frame));
    eth_write(qts, RIS0, ~FRF0);
    g_assert_false(qtest_get_irq(qts, 0));

    /* A 40-byte frame is padded to 60 and wraps through the LINKFIX */
    desc_write(qts, RX_RING + 0, DT_FEMPTY, 64, RX_BUF);
    frame_fill(frame, 40, 0x30);
    sock_send(fd, frame, 40);
    wait_rx(qts, RIS0, FRF0);
    g_assert_cmphex(qtest_readl(qts, RX_RING + 24), ==,
                    DT_FSINGLE << 28 | 1u << 13 | 60);
    eth_write(qts, RIS0, ~FRF0);
    sock_send(fd, frame, 40);
    wait_rx(qts, RIS0, FRF0);
    g_assert_cmphex(qtest_readl(qts, RX_RING + 0), ==,
                    DT_FSINGLE << 28 | 1u << 13 | 60);
    g_assert_cmphex(eth_read(qts, CDAR4), ==, RX_RING + 8);
    eth_write(qts, RIS0, ~FRF0);

    /* Descriptor 1 is still FMID, owned by the software: queue full */
    sock_send(fd, frame, 40);
    wait_rx(qts, RIS2, QFF0);
    g_assert_cmphex(eth_read(qts, RIS0), ==, 0);
    g_assert_cmphex(eth_read(qts, EIS) & QFS, ==, QFS);
    g_assert_cmphex(eth_read(qts, ISS), ==, ISS_EM);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(eth_read(qts, CDAR4), ==, RX_RING + 8);
    eth_write(qts, RIS2, ~QFF0);
    g_assert_cmphex(eth_read(qts, EIS) & QFS, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    /* Given back, the same descriptor takes the next frame */
    desc_write(qts, RX_RING + 8, DT_FEMPTY, 64, RX_BUF + 0x100);
    sock_send(fd, frame, 40);
    wait_rx(qts, RIS0, FRF0);
    g_assert_cmphex(qtest_readl(qts, RX_RING + 8), ==,
                    DT_FSINGLE << 28 | 1u << 13 | 60);
    qtest_quit(qts);
    close(fd);
}

/*
 * The TOE result (Figure 30.25) in place of the CRC: 0 for a correct
 * IPv4 header and UDP checksum; the checksum error bit for a wrong UDP
 * checksum; H'FFFF for a frame that is not IP, with the unsupported
 * frame bit (Table 30.13).
 */
static void test_rx_checksum(void)
{
    static const uint8_t udp[] = {
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x02, 0x08, 0x00,
        /* IPv4: 10.0.2.2 -> 10.0.2.15, UDP, 32 bytes, checksum 0x22bd */
        0x45, 0x00, 0x00, 0x20, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x11, 0x22, 0xbd, 0x0a, 0x00, 0x02, 0x02,
        0x0a, 0x00, 0x02, 0x0f,
        /* UDP 1234 -> 5678, 12 bytes, checksum 0x07ff, "abcd" */
        0x04, 0xd2, 0x16, 0x2e, 0x00, 0x0c, 0x07, 0xff,
        0x61, 0x62, 0x63, 0x64,
    };
    uint8_t frame[64] = { 0 }, trailer[4];
    int fd;
    QTestState *qts = gbeth_init(&fd);

    desc_write(qts, RX_RING + 0, DT_FEMPTY, 128, RX_BUF);
    desc_write(qts, RX_RING + 8, DT_FEMPTY, 128, RX_BUF + 0x100);
    desc_write(qts, RX_RING + 16, DT_FEMPTY, 128, RX_BUF + 0x200);
    desc_write(qts, RX_RING + 24, DT_LINKFIX, 0, RX_RING);
    gbeth_start(qts, ECMR_RCPT);
    eth_write(qts, CSR2, CSR2_IPV4_ALL);
    eth_write(qts, CSR0, CSR0_RPE);

    /* 60 bytes on the wire, then the 4-byte result */
    memcpy(frame, udp, sizeof(udp));
    sock_send(fd, frame, 60);
    wait_rx(qts, RIS0, FRF0);
    g_assert_cmphex(qtest_readl(qts, RX_RING + 0), ==,
                    DT_FSINGLE << 28 | 1u << 13 | 64);
    qtest_memread(qts, RX_BUF + 60, trailer, 4);
    g_assert_cmphex((uint32_t)ldl_le_p(trailer), ==, 0);
    eth_write(qts, RIS0, ~FRF0);

    frame[41] ^= 0xff;  /* break the UDP checksum */
    sock_send(fd, frame, 60);
    wait_rx(qts, RIS0, FRF0);
    g_assert_cmphex(qtest_readl(qts, RX_RING + 8) >> 16 & 0xff, ==,
                    MSC_CEEF);
    qtest_memread(qts, RX_BUF + 0x100 + 60, trailer, 4);
    g_assert_cmphex(lduw_le_p(trailer), ==, 0);
    g_assert_cmphex(lduw_le_p(trailer + 2), !=, 0);
    g_assert_cmphex(lduw_le_p(trailer + 2), !=, 0xffff);
    eth_write(qts, RIS0, ~FRF0);

    frame_fill(frame, 60, 0x40);
    sock_send(fd, frame, 60);
    wait_rx(qts, RIS0, FRF0);
    g_assert_cmphex(qtest_readl(qts, RX_RING + 16) >> 16 & 0xff, ==,
                    MSC_UNSUPPORTED);
    qtest_memread(qts, RX_BUF + 0x200 + 60, trailer, 4);
    g_assert_cmphex((uint32_t)ldl_le_p(trailer), ==, 0xffffffff);
    qtest_quit(qts);
    close(fd);
}

/*
 * A reserved descriptor type stops the queue at that descriptor with
 * EIS.QEF and ESR.ET = 2 (sections 30.4.1.6, 30.5.5.3); a data pointer
 * outside memory stops it with ESR.ET = 4 (data read). Nothing is sent.
 */
static void test_tx_errors(void)
{
    uint8_t frame[60];
    int fd;
    QTestState *qts = gbeth_init(&fd);

    gbeth_start(qts, 0);
    eth_write(qts, CXR20, ECMR_TPE | ECMR_RPE);
    desc_write(qts, TX_RING + 0, 0x0, 60, TX_BUF);
    eth_write(qts, TCCR, TSRQ0);
    g_assert_cmphex(eth_read(qts, TCCR) & TSRQ0, ==, 0);
    g_assert_cmphex(eth_read(qts, EIS) & QEF, ==, QEF);
    g_assert_cmphex(eth_read(qts, ESR), ==, ET_INTERPRET << 8 | 0);
    g_assert_cmphex(eth_read(qts, CDAR0), ==, TX_RING);
    g_assert_cmphex(qtest_readl(qts, TX_RING) >> 28, ==, 0x0);
    g_assert_cmphex(eth_read(qts, TIS), ==, 0);
    eth_write(qts, EIS, ~QEF);
    g_assert_cmphex(eth_read(qts, EIS), ==, 0);

    desc_write(qts, TX_RING + 0, DT_FSINGLE, 60, UNMAPPED);
    eth_write(qts, TCCR, TSRQ0);
    g_assert_cmphex(eth_read(qts, EIS) & QEF, ==, QEF);
    g_assert_cmphex(eth_read(qts, ESR), ==, ET_RD_DATA << 8 | 0);
    g_assert_cmphex(eth_read(qts, TIS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, TX_RING) >> 28, ==, DT_FSINGLE);

    /* The link partner got no frame: the next one is the first it sees */
    frame_fill(frame, sizeof(frame), 0x50);
    qtest_memwrite(qts, TX_BUF, frame, sizeof(frame));
    desc_write(qts, TX_RING + 0, DT_FSINGLE, 60, TX_BUF);
    desc_write(qts, TX_RING + 8, DT_EEMPTY, 0, 0);
    eth_write(qts, TCCR, TSRQ0);
    {
        uint8_t got[64];

        g_assert_cmpuint(sock_recv(fd, got, sizeof(got)), ==, 60);
        g_assert_cmpmem(got, 60, frame, 60);
    }
    qtest_quit(qts);
    close(fd);
}

/* Leaving OPERATION clears the status flags (section 30.5.1.1) */
static void test_opmode(void)
{
    uint8_t frame[60];
    int fd;
    QTestState *qts = gbeth_init(&fd);

    desc_write(qts, RX_RING + 0, DT_FEMPTY, 64, RX_BUF);
    desc_write(qts, RX_RING + 8, DT_LINKFIX, 0, RX_RING);
    gbeth_start(qts, 0);
    g_assert_cmphex(eth_read(qts, CDAR4), ==, DBAT_TABLE + 0x20);
    frame_fill(frame, sizeof(frame), 0x60);
    sock_send(fd, frame, sizeof(frame));
    wait_rx(qts, RIS0, FRF0);
    set_opc(qts, OPC_CONFIG);
    g_assert_cmphex(eth_read(qts, RIS0), ==, 0);
    g_assert_cmphex(eth_read(qts, ISS), ==, 0);
    /* DBAT is kept, and reloads the queues on the next OPERATION */
    g_assert_cmphex(eth_read(qts, DBAT), ==, DBAT_TABLE);
    set_opc(qts, OPC_OPERATION);
    g_assert_cmphex(eth_read(qts, CDAR0), ==, DBAT_TABLE);
    g_assert_cmphex(eth_read(qts, CDAR4), ==, DBAT_TABLE + 0x20);
    qtest_quit(qts);
    close(fd);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rzv2l-gbeth/reset-values", test_reset_values);
    qtest_add_func("/rzv2l-gbeth/cpg-reset", test_cpg_reset);
    qtest_add_func("/rzv2l-gbeth/mdio", test_mdio);
    qtest_add_func("/rzv2l-gbeth/link-irq", test_link_irq);
    qtest_add_func("/rzv2l-gbeth/mac-link", test_mac_link);
    qtest_add_func("/rzv2l-gbeth/tx", test_tx);
    qtest_add_func("/rzv2l-gbeth/rx", test_rx);
    qtest_add_func("/rzv2l-gbeth/rx-checksum", test_rx_checksum);
    qtest_add_func("/rzv2l-gbeth/tx-errors", test_tx_errors);
    qtest_add_func("/rzv2l-gbeth/opmode", test_opmode);

    return g_test_run();
}
