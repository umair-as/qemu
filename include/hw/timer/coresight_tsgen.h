/*
 * Arm CoreSight SoC-400 timestamp generator, control frame
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * QEMU interface:
 *  + sysbus MMIO region 0: the control frame (CNTCR, CNTSR, CNTFID0)
 *    and the read frame, 4 KiB each
 */

#ifndef HW_TIMER_CORESIGHT_TSGEN_H
#define HW_TIMER_CORESIGHT_TSGEN_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_CORESIGHT_TSGEN "coresight-tsgen"
OBJECT_DECLARE_SIMPLE_TYPE(CoreSightTSGenState, CORESIGHT_TSGEN)

#define CORESIGHT_TSGEN_MMIO_SIZE 0x2000

struct CoreSightTSGenState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    uint32_t cntcr;
    uint32_t cntfid0;
};

#endif
