/* SPDX-License-Identifier: GPL-2.0-or-later OR MIT */
#ifndef NEPTUNE_STREAM_REGS_H
#define NEPTUNE_STREAM_REGS_H

#include <neptune_pl_registers_v1.h>

/*
 * The register addresses, identity, capabilities, and bitfields above are
 * generated from specs/neptune-pl-registers-v1.json.  This driver does not
 * define a second private PL register map.  Ring descriptors are submitted to
 * the DT-provided DMAEngine channel instead of through invented AXI-Lite FIFO
 * registers.
 */
#define NEPTUNE_STREAM_REQUIRED_PL_CAPS \
	(NEPTUNE_PL_CAP_RAW_BYPASS | NEPTUNE_PL_CAP_DMA_SLOT_HEADER_V1)
#define NEPTUNE_STREAM_PL_FAULT_MASK \
	(NEPTUNE_PL_FIELD_GLOBAL_FAULTS_FIFO_OVERFLOW_MASK | \
	 NEPTUNE_PL_FIELD_GLOBAL_FAULTS_DMA_OVERRUN_MASK | \
	 NEPTUNE_PL_FIELD_GLOBAL_FAULTS_INTERFACE_ERROR_MASK)

#endif /* NEPTUNE_STREAM_REGS_H */
