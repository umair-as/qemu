/*
 * Renesas RZ family Serial Communication Interface with FIFO (SCIFA)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * One SCIFA channel in asynchronous mode (manual section 22).
 *
 * Modelled: the register file with its reset values (Table 22.4), the
 * BRR/MDDR pair that shares offset 0x02 and is selected by SEMR.MDDRS,
 * and the rule that MDDR can only be written while SCR.TE = SCR.RE = 0
 * (section 22.2.9). SPTR bits 6, 4, 2 and 0 read the input level of
 * their pins (section 22.2.12); the pins are not modelled, so RXD reads
 * as an idle line (1) and the others as 0.
 *
 * Each register accepts only its documented access width. Any other
 * access to a register's bytes is answered with a decode error. Offsets
 * without a register read as 0 and ignore writes, as observed on the
 * RZ/V2L for a 16-bit read at 0x18.
 *
 * Clock synchronous mode is not modelled: an SMR write that sets CM is
 * refused with a decode error.
 *
 * The "direct-boot" property starts the channel in the state the
 * Renesas boot firmware (TF-A) leaves the console in: 115200 8N1 with
 * the transmitter and receiver enabled, as read back on an RZ/V2L
 * board at the U-Boot prompt.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/core/sysbus.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/renesas_rz_scifa.h"
#include "migration/vmstate.h"

REG16(SMR, 0x00)
    FIELD(SMR, CKS, 0, 2)
    FIELD(SMR, STOP, 3, 1)
    FIELD(SMR, PM, 4, 1)
    FIELD(SMR, PE, 5, 1)
    FIELD(SMR, CHR, 6, 1)
    FIELD(SMR, CM, 7, 1)
REG8(BRR, 0x02)               /* MDDR at the same offset (SEMR.MDDRS) */
REG16(SCR, 0x04)
    FIELD(SCR, CKE, 0, 2)
    FIELD(SCR, TEIE, 2, 1)
    FIELD(SCR, REIE, 3, 1)
    FIELD(SCR, RE, 4, 1)
    FIELD(SCR, TE, 5, 1)
    FIELD(SCR, RIE, 6, 1)
    FIELD(SCR, TIE, 7, 1)
REG8(FTDR, 0x06)
REG16(FSR, 0x08)
    FIELD(FSR, DR, 0, 1)
    FIELD(FSR, RDF, 1, 1)
    FIELD(FSR, PER, 2, 1)
    FIELD(FSR, FER, 3, 1)
    FIELD(FSR, BRK, 4, 1)
    FIELD(FSR, TDFE, 5, 1)
    FIELD(FSR, TEND, 6, 1)
    FIELD(FSR, ER, 7, 1)
REG8(FRDR, 0x0a)
REG16(FCR, 0x0c)
    FIELD(FCR, LOOP, 0, 1)
    FIELD(FCR, RFRST, 1, 1)
    FIELD(FCR, TFRST, 2, 1)
    FIELD(FCR, MCE, 3, 1)
    FIELD(FCR, TTRG, 4, 2)
    FIELD(FCR, RTRG, 6, 2)
    FIELD(FCR, RSTRG, 8, 3)
REG16(FDR, 0x0e)
    FIELD(FDR, R, 0, 5)
    FIELD(FDR, T, 8, 5)
REG16(SPTR, 0x10)
    FIELD(SPTR, SPB2DT, 0, 1)
REG16(LSR, 0x12)
    FIELD(LSR, ORER, 0, 1)
REG8(SEMR, 0x14)
    FIELD(SEMR, ABCS0, 0, 1)
    FIELD(SEMR, NFEN, 2, 1)
    FIELD(SEMR, DIR, 3, 1)
    FIELD(SEMR, MDDRS, 4, 1)
    FIELD(SEMR, BRME, 5, 1)
    FIELD(SEMR, BGDM, 7, 1)
REG16(FTCR, 0x16)
    FIELD(FTCR, TFTC, 0, 5)
    FIELD(FTCR, TTRGS, 7, 1)
    FIELD(FTCR, RFTC, 8, 5)
    FIELD(FTCR, RTRGS, 15, 1)

/* Writable bits; the others are reserved and read as 0 */
#define SMR_WMASK   0x00fb
#define SCR_WMASK   0x00ff
#define FCR_WMASK   0x07ff
#define SPTR_WMASK  0x00ff
#define SEMR_WMASK  0xbd
#define FTCR_WMASK  0x9f9f

/* SPTR bits that read the pin input level instead of the written value */
#define SPTR_PIN_BITS 0x0055

/* MDDR bit 7 is fixed to 1 (section 22.2.9) */
#define MDDR_FIXED  0x80

/* Reset values, Table 22.4 */
#define SMR_RESET   0x0000
#define BRR_RESET   0xff
#define MDDR_RESET  0xff
#define SCR_RESET   0x0000
#define FSR_RESET   0x0020
#define FCR_RESET   0x0000
#define LSR_RESET   0x0000
#define SEMR_RESET  0x00
#define FTCR_RESET  0x1f1f

/*
 * Start state for "direct-boot": what TF-A's console_rz_init() leaves
 * behind (BRR 0x1a, then SEMR = MDDRS | BRME and MDDR 0xfe, SCR = TE |
 * RE), with FSR showing an idle transmitter. All but BRR were read back
 * on an RZ/V2L board; BRR is hidden behind MDDR while MDDRS = 1.
 */
#define BRR_DIRECT  0x1a
#define MDDR_DIRECT 0xfe
#define SCR_DIRECT  0x0030
#define FSR_DIRECT  0x0060
#define SEMR_DIRECT 0x30

typedef struct {
    hwaddr offset;
    unsigned size;
} RZSCIFAReg;

static const RZSCIFAReg rz_scifa_regs[] = {
    { A_SMR, 2 }, { A_BRR, 1 }, { A_SCR, 2 }, { A_FTDR, 1 },
    { A_FSR, 2 }, { A_FRDR, 1 }, { A_FCR, 2 }, { A_FDR, 2 },
    { A_SPTR, 2 }, { A_LSR, 2 }, { A_SEMR, 1 }, { A_FTCR, 2 },
};

typedef enum {
    RZ_SCIFA_ACCESS_OK,         /* a whole register at its width */
    RZ_SCIFA_ACCESS_BAD,        /* part of a register, or the wrong width */
    RZ_SCIFA_ACCESS_HOLE,       /* no register at these bytes */
} RZSCIFAAccess;

static RZSCIFAAccess rz_scifa_classify(hwaddr offset, unsigned size)
{
    for (int i = 0; i < ARRAY_SIZE(rz_scifa_regs); i++) {
        const RZSCIFAReg *r = &rz_scifa_regs[i];

        if (offset < r->offset + r->size && r->offset < offset + size) {
            return (offset == r->offset && size == r->size) ?
                   RZ_SCIFA_ACCESS_OK : RZ_SCIFA_ACCESS_BAD;
        }
    }
    return RZ_SCIFA_ACCESS_HOLE;
}

static uint16_t rz_scifa_sptr_read(RenesasRZSCIFAState *s)
{
    uint16_t pins = R_SPTR_SPB2DT_MASK;   /* RXD idle (mark) */

    return (s->sptr & ~SPTR_PIN_BITS) | pins;
}

static MemTxResult rz_scifa_read(void *opaque, hwaddr offset,
                                 uint64_t *data, unsigned size,
                                 MemTxAttrs attrs)
{
    RenesasRZSCIFAState *s = RENESAS_RZ_SCIFA(opaque);
    uint64_t r;

    switch (rz_scifa_classify(offset, size)) {
    case RZ_SCIFA_ACCESS_BAD:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad %u-byte read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, size, offset);
        return MEMTX_DECODE_ERROR;
    case RZ_SCIFA_ACCESS_HOLE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read at undefined offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        *data = 0;
        return MEMTX_OK;
    case RZ_SCIFA_ACCESS_OK:
        break;
    }

    switch (offset) {
    case A_SMR:
        r = s->smr;
        break;
    case A_BRR:
        r = FIELD_EX8(s->semr, SEMR, MDDRS) ? s->mddr : s->brr;
        break;
    case A_SCR:
        r = s->scr;
        break;
    case A_FSR:
        r = s->fsr;
        break;
    case A_FCR:
        r = s->fcr;
        break;
    case A_FDR:
        r = 0;
        break;
    case A_SPTR:
        r = rz_scifa_sptr_read(s);
        break;
    case A_LSR:
        r = s->lsr;
        break;
    case A_SEMR:
        r = s->semr;
        break;
    case A_FTCR:
        r = s->ftcr;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }

    trace_renesas_rz_scifa_read(offset, r, size);
    *data = r;
    return MEMTX_OK;
}

static MemTxResult rz_scifa_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size,
                                  MemTxAttrs attrs)
{
    RenesasRZSCIFAState *s = RENESAS_RZ_SCIFA(opaque);

    switch (rz_scifa_classify(offset, size)) {
    case RZ_SCIFA_ACCESS_BAD:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad %u-byte write at offset 0x%" HWADDR_PRIx "\n",
                      __func__, size, offset);
        return MEMTX_DECODE_ERROR;
    case RZ_SCIFA_ACCESS_HOLE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write at undefined offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_OK;
    case RZ_SCIFA_ACCESS_OK:
        break;
    }

    trace_renesas_rz_scifa_write(offset, value, size);

    switch (offset) {
    case A_SMR:
        if (FIELD_EX16(value, SMR, CM)) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: clock synchronous mode not supported\n",
                          __func__);
            return MEMTX_DECODE_ERROR;
        }
        s->smr = value & SMR_WMASK;
        break;
    case A_BRR:
        if (!FIELD_EX8(s->semr, SEMR, MDDRS)) {
            s->brr = value;
        } else if (FIELD_EX16(s->scr, SCR, TE) ||
                   FIELD_EX16(s->scr, SCR, RE)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: MDDR write ignored while TE or RE is set\n",
                          __func__);
        } else {
            s->mddr = value | MDDR_FIXED;
        }
        break;
    case A_SCR:
        s->scr = value & SCR_WMASK;
        break;
    case A_FCR:
        s->fcr = value & FCR_WMASK;
        break;
    case A_FDR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only FDR\n", __func__);
        break;
    case A_SPTR:
        s->sptr = value & SPTR_WMASK;
        break;
    case A_SEMR:
        s->semr = value & SEMR_WMASK;
        break;
    case A_FTCR:
        s->ftcr = value & FTCR_WMASK;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps rz_scifa_ops = {
    .read_with_attrs = rz_scifa_read,
    .write_with_attrs = rz_scifa_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static void rz_scifa_reset_hold(Object *obj, ResetType type)
{
    RenesasRZSCIFAState *s = RENESAS_RZ_SCIFA(obj);

    s->smr = SMR_RESET;
    s->brr = BRR_RESET;
    s->mddr = MDDR_RESET;
    s->scr = SCR_RESET;
    s->fsr = FSR_RESET;
    s->fcr = FCR_RESET;
    s->sptr = 0;
    s->lsr = LSR_RESET;
    s->semr = SEMR_RESET;
    s->ftcr = FTCR_RESET;

    if (s->direct_boot) {
        s->brr = BRR_DIRECT;
        s->mddr = MDDR_DIRECT;
        s->scr = SCR_DIRECT;
        s->fsr = FSR_DIRECT;
        s->semr = SEMR_DIRECT;
    }
}

static void rz_scifa_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RenesasRZSCIFAState *s = RENESAS_RZ_SCIFA(obj);

    memory_region_init_io(&s->iomem, obj, &rz_scifa_ops, s,
                          TYPE_RENESAS_RZ_SCIFA, RENESAS_RZ_SCIFA_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rz_scifa = {
    .name = TYPE_RENESAS_RZ_SCIFA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(smr, RenesasRZSCIFAState),
        VMSTATE_UINT8(brr, RenesasRZSCIFAState),
        VMSTATE_UINT8(mddr, RenesasRZSCIFAState),
        VMSTATE_UINT16(scr, RenesasRZSCIFAState),
        VMSTATE_UINT16(fsr, RenesasRZSCIFAState),
        VMSTATE_UINT16(fcr, RenesasRZSCIFAState),
        VMSTATE_UINT16(sptr, RenesasRZSCIFAState),
        VMSTATE_UINT16(lsr, RenesasRZSCIFAState),
        VMSTATE_UINT8(semr, RenesasRZSCIFAState),
        VMSTATE_UINT16(ftcr, RenesasRZSCIFAState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property rz_scifa_props[] = {
    DEFINE_PROP_BOOL("direct-boot", RenesasRZSCIFAState, direct_boot, false),
};

static void rz_scifa_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = rz_scifa_reset_hold;
    dc->vmsd = &vmstate_rz_scifa;
    device_class_set_props(dc, rz_scifa_props);
}

static const TypeInfo rz_scifa_info = {
    .name = TYPE_RENESAS_RZ_SCIFA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRZSCIFAState),
    .instance_init = rz_scifa_init,
    .class_init = rz_scifa_class_init,
};

static void rz_scifa_register_types(void)
{
    type_register_static(&rz_scifa_info);
}

type_init(rz_scifa_register_types);
