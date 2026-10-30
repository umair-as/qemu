/*
 * Renesas RZ/G2L family Gigabit Ethernet interface (GbEthernet)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The Gigabit Ethernet interface of the Renesas RZ/G2L family (RZ/G2L,
 * RZ/V2L): DMAC, E-MAC and TCP/IP checksum offload engine. RZ/V2L Group
 * User's Manual: Hardware (R01UH0936EJ), chapter 30.
 *
 * QEMU interface:
 *  + sysbus MMIO region 0: the registers
 *  + sysbus IRQs 0-2: pif_int_n, int_fil_n, int_arp_ns_n, active high
 *  + named GPIO input "reset": high while ETHn_RST_HW_N holds the
 *    interface in reset
 *  + named GPIO input "phy-link": the link state of the PHY, as the
 *    RGMII in-band status and the LINKSTA pin report it
 *  + QOM link "dma-memory": the address space the DMAC sees
 *  + QOM link "phy": the PHY on the MDIO bus, at address "phy-addr"
 *  + a NIC; its link state is the cable of the PHY
 */

#ifndef HW_NET_RENESAS_GBETH_H
#define HW_NET_RENESAS_GBETH_H

#include "hw/core/sysbus.h"
#include "hw/net/ksz9131_phy.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_RENESAS_GBETH "renesas-gbeth"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasGbEthState, RENESAS_GBETH)

#define RENESAS_GBETH_MMIO_SIZE 0x10000
#define RENESAS_GBETH_NUM_IRQ 3
/* Table 30.1: frames up to 1.5 KB are transmitted, 8 KB received */
#define RENESAS_GBETH_TX_FRAME_MAX 1536
#define RENESAS_GBETH_RX_FRAME_MAX 8192

struct RenesasGbEthState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq[RENESAS_GBETH_NUM_IRQ];
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    NICState *nic;
    NICConf conf;
    KSZ9131PhyState *phy;
    uint8_t phy_addr;

    bool in_reset;
    bool phy_link;
    bool link;

    /* DMAC */
    uint32_t ccc;
    uint32_t dbat;
    uint32_t cdar_tx;
    uint32_t cdar_rx;
    uint32_t esr;
    uint32_t rcr;
    uint32_t rtc;
    uint32_t tgc;
    uint32_t tccr;
    uint32_t dic;
    uint32_t dis;
    uint32_t eic;
    uint32_t eis;
    uint32_t ric0;
    uint32_t ris0;
    uint32_t ric1;
    uint32_t ric2;
    uint32_t ris2;
    uint32_t tic;
    uint32_t tis;
    uint32_t cie;
    uint32_t ric3;
    uint32_t ris3;

    /* E-MAC */
    uint32_t ecmr;
    uint32_t rflr;
    uint32_t ecsr;
    uint32_t ecsipr;
    uint32_t pir;
    uint32_t pipr;
    uint32_t cxr31;
    uint32_t cxr35;
    uint32_t cxr71;
    uint32_t cxr81;
    uint32_t gecmr;
    uint32_t cxr2g;
    uint32_t mahr;
    uint32_t malr;
    uint32_t mdiomod;
    uint32_t mc_count;

    /* TOE */
    uint32_t csr0;
    uint32_t csr1;
    uint32_t csr2;

    /* MDIO frame decoder on CXR23 */
    uint8_t mdio_state;
    uint8_t mdio_bits;
    uint16_t mdio_shift;
    uint16_t mdio_data;
    uint8_t mdio_reg;
    bool mdio_respond;
    bool mdio_out;

    /* A frame being gathered from transmission descriptors */
    uint8_t tx_frame[RENESAS_GBETH_TX_FRAME_MAX];
    uint32_t tx_len;
    bool tx_in_frame;
    bool tx_oversize;
};

#endif
