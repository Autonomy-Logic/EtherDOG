// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file layout.h
 * @brief Process data layout: where each configured PDO entry sits in the exchanged image.
 *
 * Built once per start, after SOEM has mapped the group. Clients read it through the
 * "layout" command and address data by (slave position, entry index, subindex).
 */

#ifndef EDOG_LAYOUT_H
#define EDOG_LAYOUT_H

#include "config.h"
#include "log.h"

/**
 * @brief Build @p inst->layout from the configured PDOs and SOEM's group 0 mapping.
 *
 * Every non-padding entry of every configured RxPDO (outputs) and TxPDO (inputs) is
 * published, whether or not a channel names it. Offsets are bit offsets relative to the
 * start of the entry's direction region.
 *
 * @return 0 on success, -1 if a slave is missing from SOEM, the layout overflows, or a
 *         region does not fit in one data frame.
 */
int ecat_layout_build(ecat_master_instance_t *inst, edog_logger_t *logger);

/** Pointer to the start of the output region (IOmap), or NULL before mapping. */
uint8_t *ecat_layout_output_region(ecat_master_instance_t *inst);

/** Pointer to the start of the input region, or NULL before mapping. */
uint8_t *ecat_layout_input_region(ecat_master_instance_t *inst);

#endif /* EDOG_LAYOUT_H */
