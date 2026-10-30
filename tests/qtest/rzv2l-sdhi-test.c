/*
 * QTest testcase for the SD/MMC host interface (SDHI) of the Renesas RZ/V2L
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Expected values come from the RZ/V2L Group User's Manual: Hardware
 * (R01UH0936EJ0150), section 40, and from the card image the test
 * writes, never from the model. Without a kernel the machine starts at
 * the manual reset values: CPG_RST_SDHI holds both channels in reset
 * (section 7.2.4, reset value 0) until the test releases them. The card
 * on SDHI0 is a QEMU SD card backed by a temporary 1 MiB image (a
 * standard capacity card: byte addresses).
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define SDHI0_BASE          0x11c00000
#define SDHI1_BASE          0x11c10000

/* Table 40.2 */
#define SD_CMD              0x000
#define SD_ARG              0x010
#define SD_STOP             0x020
#define SD_SECCNT           0x028
#define SD_RSP10            0x030
#define SD_RSP54            0x050
#define SD_INFO1            0x070
#define SD_INFO2            0x078
#define SD_INFO1_MASK       0x080
#define SD_INFO2_MASK       0x088
#define SD_CLK_CTRL         0x090
#define SD_SIZE             0x098
#define SD_OPTION           0x0a0
#define SD_ERR_STS1         0x0b0
#define SD_ERR_STS2         0x0b8
#define SD_BUF0             0x0c0
#define SDIO_INFO1_MASK     0x0e0
#define CC_EXT_MODE         0x360
#define SOFT_RST            0x380
#define VERSION             0x388
#define HOST_MODE           0x390
#define DM_CM_DTRAN_MODE    0x820
#define DM_CM_DTRAN_CTRL    0x828
#define DM_CM_RST           0x830
#define DM_CM_INFO1         0x840
#define DM_CM_INFO1_MASK    0x848
#define DM_CM_INFO2_MASK    0x858
#define DM_DTRAN_ADDR       0x880

/* Section 40.2 reset values */
#define SD_INFO1_MASK_RESET     0x0001031d
#define SD_INFO2_MASK_RESET     0x00008b7f
#define SD_CLK_CTRL_RESET       0x00000020
#define SD_SIZE_RESET           0x00000200
#define SD_OPTION_RESET         0x000040ee
#define SD_ERR_STS1_RESET       0x00002000
#define SDIO_INFO1_MASK_RESET   0x0000c007
#define CC_EXT_MODE_RESET       0x00001010
#define SOFT_RST_RESET          0x00000007
#define VERSION_RESET           0x0000cc10
#define DM_CM_DTRAN_MODE_RESET  0x00000030

/* SD_INFO1 and SD_INFO2 bits (sections 40.2.6, 40.2.7) */
#define INFO0               (1u << 0)   /* response end */
#define INFO2               (1u << 2)   /* access end */
#define INFO5               (1u << 5)   /* card detect pin */
#define ERR6                (1u << 6)   /* response timeout */
#define BRE                 (1u << 8)
#define BWE                 (1u << 9)
#define SCLKDIVEN           (1u << 13)
#define CBSY                (1u << 14)
/* DM_CM_INFO1: channel 1 is "SD up stream", card to memory (40.2.25, 28) */
#define DTRANEND1           (1u << 20)
#define DTRANEND0           (1u << 16)

/* SD_CMD extended mode (section 40.2.1) */
#define CMD_NO_RSP          0x0300
#define CMD_R1              0x0400
#define CMD_R1B             0x0500
#define CMD_R2              0x0600
#define CMD_R3              0x0700
#define CMD_DATA            0x0800
#define CMD_READ            0x1000
#define CMD_MULTI           0x2000

#define CPG_BASE            0x11010000
#define CPG_RST_SDHI        0x854

#define SDHI_SPI0           104 /* Table 8.2: OXMNIRQ0; OXMNIRQ1 is 106 */
#define DMA_BUF             0x50000000
#define CARD_SIZE           (1024 * 1024)

static uint32_t rd(QTestState *qts, uint32_t base, uint32_t reg)
{
    return qtest_readl(qts, base + reg);
}

static void wr(QTestState *qts, uint32_t base, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, base + reg, v);
}

/* Release both channels from the CPG reset; enable the SD clock */
static void sdhi_release(QTestState *qts, uint32_t base)
{
    qtest_writel(qts, CPG_BASE + CPG_RST_SDHI, 0x00030003);
    wr(qts, base, SD_CLK_CTRL, 0x100 | 0x80);
}

/* Clear every SD_INFO1/2 flag (write 0 to clear, section 40.2.6) */
static void clear_info(QTestState *qts, uint32_t base)
{
    wr(qts, base, SD_INFO1, 0);
    wr(qts, base, SD_INFO2, 1u << 11);
}

static uint32_t cmd(QTestState *qts, uint32_t base, uint32_t c, uint32_t arg)
{
    clear_info(qts, base);
    wr(qts, base, SD_ARG, arg);
    wr(qts, base, SD_CMD, c);
    g_assert_cmphex(rd(qts, base, SD_INFO1) & INFO0, ==, INFO0);
    g_assert_cmphex(rd(qts, base, SD_INFO2) & ERR6, ==, 0);
    return rd(qts, base, SD_RSP10);
}

/* SD card identification and selection (SD Physical Layer 4.2) */
static void card_init(QTestState *qts, uint32_t base)
{
    uint32_t ocr, rca;
    int i;

    cmd(qts, base, CMD_NO_RSP | 0, 0);
    cmd(qts, base, CMD_R1 | 8, 0x1aa);
    for (i = 0; i < 100; i++) {
        cmd(qts, base, CMD_R1 | 55, 0);
        ocr = cmd(qts, base, CMD_R3 | 41, 0x00300000);
        if (ocr & (1u << 31)) {
            break;
        }
        qtest_clock_step(qts, 1000000);
    }
    g_assert_cmphex(ocr & (1u << 31), !=, 0);
    cmd(qts, base, CMD_R2 | 2, 0);
    rca = cmd(qts, base, CMD_R1 | 3, 0) & 0xffff0000;
    cmd(qts, base, CMD_R1B | 7, rca);
}

static char *make_card(void)
{
    char *path = NULL;
    int fd = g_file_open_tmp("sdhi-test-XXXXXX.img", &path, NULL);
    g_autofree uint8_t *data = g_malloc(CARD_SIZE);
    int i;

    g_assert(fd >= 0);
    for (i = 0; i < CARD_SIZE; i++) {
        data[i] = (i * 7 + (i >> 9)) & 0xff;
    }
    g_assert(write(fd, data, CARD_SIZE) == CARD_SIZE);
    close(fd);
    return path;
}

static uint8_t card_byte(uint32_t i)
{
    return (i * 7 + (i >> 9)) & 0xff;
}

static QTestState *start_with_card(const char *img)
{
    return qtest_initf("-machine rzv2l-smarc "
                       "-drive if=sd,index=0,format=raw,file=%s", img);
}

static void test_reset_values(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    uint32_t b = SDHI0_BASE;

    qtest_writel(qts, CPG_BASE + CPG_RST_SDHI, 0x00030003);
    g_assert_cmphex(rd(qts, b, SD_INFO1_MASK), ==, SD_INFO1_MASK_RESET);
    g_assert_cmphex(rd(qts, b, SD_INFO2_MASK), ==, SD_INFO2_MASK_RESET);
    g_assert_cmphex(rd(qts, b, SD_CLK_CTRL), ==, SD_CLK_CTRL_RESET);
    g_assert_cmphex(rd(qts, b, SD_SIZE), ==, SD_SIZE_RESET);
    g_assert_cmphex(rd(qts, b, SD_OPTION), ==, SD_OPTION_RESET);
    g_assert_cmphex(rd(qts, b, SD_ERR_STS1), ==, SD_ERR_STS1_RESET);
    g_assert_cmphex(rd(qts, b, SDIO_INFO1_MASK), ==, SDIO_INFO1_MASK_RESET);
    g_assert_cmphex(rd(qts, b, CC_EXT_MODE), ==, CC_EXT_MODE_RESET);
    g_assert_cmphex(rd(qts, b, SOFT_RST), ==, SOFT_RST_RESET);
    g_assert_cmphex(rd(qts, b, VERSION), ==, VERSION_RESET);
    g_assert_cmphex(rd(qts, b, HOST_MODE), ==, 0);
    g_assert_cmphex(rd(qts, b, DM_CM_DTRAN_MODE), ==, DM_CM_DTRAN_MODE_RESET);
    g_assert_cmphex(rd(qts, b, DM_CM_RST), ==, 0xffffffff);
    g_assert_cmphex(rd(qts, b, DM_CM_INFO1_MASK), ==, 0xffffffff);
    g_assert_cmphex(rd(qts, b, DM_CM_INFO2_MASK), ==, 0xffffffff);
    /* SCLKDIVEN = 1, CBSY = 0; bit 7 (DAT0) is a pin */
    g_assert_cmphex(rd(qts, b, SD_INFO2) & ~0x80u, ==, SCLKDIVEN);
    /* The 16-bit view of a register and its mirror (Table 40.2) */
    g_assert_cmphex(qtest_readw(qts, b + VERSION), ==, VERSION_RESET);
    wr(qts, b, SD_ARG, 0x12345678);
    g_assert_cmphex(qtest_readw(qts, b + SD_ARG + 8), ==, 0x1234);
    qtest_quit(qts);
}

/* CPG_RST_SDHI holds the channel in reset: writes do not take */
static void test_cpg_reset(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");

    wr(qts, SDHI0_BASE, SD_SIZE, 0x40);
    g_assert_cmphex(rd(qts, SDHI0_BASE, SD_SIZE), ==, SD_SIZE_RESET);
    qtest_writel(qts, CPG_BASE + CPG_RST_SDHI, 0x00010001);
    wr(qts, SDHI0_BASE, SD_SIZE, 0x40);
    g_assert_cmphex(rd(qts, SDHI0_BASE, SD_SIZE), ==, 0x40);
    /* Applying the reset again returns the reset value */
    qtest_writel(qts, CPG_BASE + CPG_RST_SDHI, 0x00010000);
    g_assert_cmphex(rd(qts, SDHI0_BASE, SD_SIZE), ==, SD_SIZE_RESET);
    qtest_quit(qts);
}

/* No card on SDHI1: response timeout, the sequence ends, OXMNIRQ1 */
static void test_no_card(void)
{
    QTestState *qts = qtest_init("-machine rzv2l-smarc");
    uint32_t b = SDHI1_BASE;

    qtest_irq_intercept_out_named(qts, "/machine/soc/sdhi1", "sysbus-irq");
    sdhi_release(qts, b);
    g_assert_cmphex(rd(qts, b, SD_INFO1) & INFO5, ==, 0);
    wr(qts, b, SD_INFO1_MASK, ~INFO0 & SD_INFO1_MASK_RESET);
    wr(qts, b, SD_CMD, CMD_R1 | 8);
    g_assert_cmphex(rd(qts, b, SD_INFO1) & INFO0, ==, INFO0);
    g_assert_cmphex(rd(qts, b, SD_INFO2) & (ERR6 | CBSY | SCLKDIVEN), ==,
                    ERR6 | SCLKDIVEN);
    g_assert_cmphex(rd(qts, b, SD_ERR_STS2) & 1, ==, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    /* Write 0 to clear, 1 to keep (section 40.2.7) */
    wr(qts, b, SD_INFO2, ~ERR6);
    g_assert_cmphex(rd(qts, b, SD_INFO2) & ERR6, ==, 0);
    g_assert_cmphex(rd(qts, b, SD_INFO1) & INFO0, ==, INFO0);
    wr(qts, b, SD_INFO1, ~INFO0);
    g_assert_cmphex(rd(qts, b, SD_INFO1) & INFO0, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

/* One block through SD_BUF0: CMD17 read, CMD24 write */
static void test_pio(void)
{
    g_autofree char *img = make_card();
    QTestState *qts = start_with_card(img);
    uint32_t b = SDHI0_BASE;
    uint8_t blk[512], back[512];
    int i;

    sdhi_release(qts, b);
    g_assert_cmphex(rd(qts, b, SD_INFO1) & INFO5, ==, INFO5);
    card_init(qts, b);

    cmd(qts, b, CMD_R1 | CMD_DATA | CMD_READ | 17, 0x400);
    g_assert_cmphex(rd(qts, b, SD_INFO2) & (BRE | CBSY), ==, BRE | CBSY);
    wr(qts, b, SD_INFO2, ~BRE);
    for (i = 0; i < 512; i += 2) {
        uint16_t w = qtest_readw(qts, b + SD_BUF0);

        g_assert_cmphex(w & 0xff, ==, card_byte(0x400 + i));
        g_assert_cmphex(w >> 8, ==, card_byte(0x400 + i + 1));
    }
    g_assert_cmphex(rd(qts, b, SD_INFO1) & INFO2, ==, INFO2);
    g_assert_cmphex(rd(qts, b, SD_INFO2) & CBSY, ==, 0);

    for (i = 0; i < 512; i++) {
        blk[i] = 0xff - i;
    }
    cmd(qts, b, CMD_R1 | CMD_DATA | 24, 0x800);
    g_assert_cmphex(rd(qts, b, SD_INFO2) & BWE, ==, BWE);
    wr(qts, b, SD_INFO2, ~BWE);
    for (i = 0; i < 512; i += 8) {
        uint64_t q;

        memcpy(&q, blk + i, 8);
        qtest_writeq(qts, b + SD_BUF0, le64_to_cpu(q));
    }
    g_assert_cmphex(rd(qts, b, SD_INFO1) & INFO2, ==, INFO2);
    qtest_quit(qts);

    /* The block is in the image */
    {
        FILE *f = fopen(img, "rb");

        g_assert(f);
        g_assert(fseek(f, 0x800, SEEK_SET) == 0);
        g_assert(fread(back, 1, 512, f) == 512);
        fclose(f);
        g_assert(memcmp(blk, back, 512) == 0);
    }
    unlink(img);
}

/* Built-in DMAC: CMD18 and CMD25 with SD_SECCNT and the automatic CMD12 */
static void test_dma(void)
{
    g_autofree char *img = make_card();
    QTestState *qts = start_with_card(img);
    uint32_t b = SDHI0_BASE;
    g_autofree uint8_t *mem = g_malloc(3 * 512);
    g_autofree uint8_t *back = g_malloc(3 * 512);
    uint32_t rsp54;
    int i;

    qtest_irq_intercept_out_named(qts, "/machine/soc/sdhi0", "sysbus-irq");
    sdhi_release(qts, b);
    card_init(qts, b);

    /* Read three blocks from 0x1000 into memory (Figure 40.22) */
    wr(qts, b, SD_STOP, 0x100);
    wr(qts, b, SD_SECCNT, 3);
    wr(qts, b, CC_EXT_MODE, 0x1012);
    cmd(qts, b, CMD_R1 | CMD_DATA | CMD_READ | CMD_MULTI | 18, 0x1000);
    g_assert_cmphex(rd(qts, b, SD_INFO2) & (CBSY | BRE), ==, CBSY);
    wr(qts, b, DM_CM_INFO1_MASK, ~DTRANEND1);
    wr(qts, b, DM_CM_DTRAN_MODE, 0x00010030);
    wr(qts, b, DM_DTRAN_ADDR, DMA_BUF);
    wr(qts, b, DM_CM_DTRAN_CTRL, 1);
    g_assert_cmphex(rd(qts, b, DM_CM_DTRAN_CTRL), ==, 0);
    g_assert_cmphex(rd(qts, b, DM_CM_INFO1) & (DTRANEND1 | DTRANEND0), ==,
                    DTRANEND1);
    g_assert_cmphex(rd(qts, b, SD_INFO1) & INFO2, ==, INFO2);
    g_assert_cmphex(rd(qts, b, SD_INFO2) & CBSY, ==, 0);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_memread(qts, DMA_BUF, mem, 3 * 512);
    for (i = 0; i < 3 * 512; i++) {
        g_assert_cmphex(mem[i], ==, card_byte(0x1000 + i));
    }
    /*
     * Table 40.3: the CMD18 response stays in R[103:72] (SD_RSP54) while
     * the automatic CMD12 response is in SD_RSP10. Card states (SD
     * Physical Layer, card status bits 12:9): CMD18 is accepted in
     * "tran" (4), CMD12 in "data" (5).
     */
    rsp54 = rd(qts, b, SD_RSP54);
    g_assert_cmphex((rsp54 >> 9) & 0xf, ==, 4);
    g_assert_cmphex((rd(qts, b, SD_RSP10) >> 9) & 0xf, ==, 5);
    wr(qts, b, DM_CM_INFO1, ~DTRANEND1);
    g_assert_cmphex(rd(qts, b, DM_CM_INFO1) & DTRANEND1, ==, 0);

    /* Write three blocks from memory to 0x4000 (Figure 40.23) */
    for (i = 0; i < 3 * 512; i++) {
        mem[i] = i * 13 + 5;
    }
    qtest_memwrite(qts, DMA_BUF, mem, 3 * 512);
    cmd(qts, b, CMD_R1 | CMD_DATA | CMD_MULTI | 25, 0x4000);
    g_assert_cmphex(rd(qts, b, SD_INFO2) & (CBSY | BWE), ==, CBSY);
    wr(qts, b, DM_CM_DTRAN_MODE, 0x00000030);
    wr(qts, b, DM_DTRAN_ADDR, DMA_BUF);
    wr(qts, b, DM_CM_DTRAN_CTRL, 1);
    g_assert_cmphex(rd(qts, b, DM_CM_INFO1) & (DTRANEND1 | DTRANEND0), ==,
                    DTRANEND0);
    g_assert_cmphex(rd(qts, b, SD_INFO1) & INFO2, ==, INFO2);
    qtest_quit(qts);

    {
        FILE *f = fopen(img, "rb");

        g_assert(f);
        g_assert(fseek(f, 0x4000, SEEK_SET) == 0);
        g_assert(fread(back, 1, 3 * 512, f) == 3 * 512);
        fclose(f);
        g_assert(memcmp(mem, back, 3 * 512) == 0);
    }
    unlink(img);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rzv2l-sdhi/reset-values", test_reset_values);
    qtest_add_func("/rzv2l-sdhi/cpg-reset", test_cpg_reset);
    qtest_add_func("/rzv2l-sdhi/no-card", test_no_card);
    qtest_add_func("/rzv2l-sdhi/pio", test_pio);
    qtest_add_func("/rzv2l-sdhi/dma", test_dma);
    return g_test_run();
}
