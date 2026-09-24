// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file layout.c
 * @brief Walks each slave's configured PDOs over SOEM's mapping to publish entry offsets.
 *
 * SOEM maps group 0 without overlap: outputs occupy IOmap[0, Obytes), inputs follow at
 * IOmap[Obytes, Obytes + Ibytes). Each slave's slice starts at its outputs/inputs pointer
 * plus Ostartbit/Istartbit, and its entries follow in configured PDO order.
 */

#include "layout.h"
#include "master.h"

#include <stdio.h>
#include <string.h>

static bool is_padding(const ecat_pdo_entry_t *entry)
{
    return entry->parsed_type == ECAT_DTYPE_PAD || strcmp(entry->index, "0x0000") == 0 ||
           strcmp(entry->index, "0x0") == 0;
}

uint8_t *ecat_layout_output_region(ecat_master_instance_t *inst)
{
    if (!inst->soem_initialized)
        return NULL;
    return inst->ecx_context.grouplist[0].outputs;
}

uint8_t *ecat_layout_input_region(ecat_master_instance_t *inst)
{
    if (!inst->soem_initialized)
        return NULL;
    return inst->ecx_context.grouplist[0].inputs;
}

/* Publish one direction of one slave. Returns 0 or -1 on overflow. */
static int add_direction(ecat_master_instance_t *inst, const ecat_slave_t *cfg,
                         const ec_slavet *soem, bool is_output, edog_logger_t *logger)
{
    const ecat_pdo_t *pdos = is_output ? cfg->rx_pdos : cfg->tx_pdos;
    int pdo_count = is_output ? cfg->rx_pdo_count : cfg->tx_pdo_count;
    const uint8_t *slave_base = is_output ? soem->outputs : soem->inputs;
    const uint8_t *region = is_output ? ecat_layout_output_region(inst)
                                      : ecat_layout_input_region(inst);
    int start_bit = is_output ? soem->Ostartbit : soem->Istartbit;

    if (pdo_count == 0)
        return 0;
    if (slave_base == NULL || region == NULL) {
        edog_log_warn(logger,
                      "Master '%s': slave %d (%s) has configured %s PDOs but no %s data "
                      "was mapped by the bus",
                      inst->name, cfg->position, cfg->name, is_output ? "Rx" : "Tx",
                      is_output ? "output" : "input");
        return 0;
    }

    uint32_t bit = (uint32_t)((slave_base - region) * 8) + (uint32_t)start_bit;
    ecat_layout_t *layout = &inst->layout;

    for (int p = 0; p < pdo_count; p++) {
        const ecat_pdo_t *pdo = &pdos[p];
        for (int e = 0; e < pdo->entry_count; e++) {
            const ecat_pdo_entry_t *entry = &pdo->entries[e];
            if (!is_padding(entry)) {
                if (layout->entry_count >= ECAT_MAX_LAYOUT_ENTRIES) {
                    edog_log_error(logger, "Master '%s': layout full (%d entries)", inst->name,
                                   ECAT_MAX_LAYOUT_ENTRIES);
                    return -1;
                }
                ecat_layout_entry_t *out = &layout->entries[layout->entry_count++];
                memset(out, 0, sizeof(*out));
                out->slave_position = cfg->position;
                memcpy(out->pdo_index, pdo->index, sizeof(out->pdo_index));
                out->pdo_index[sizeof(out->pdo_index) - 1] = '\0';
                memcpy(out->entry_index, entry->index, sizeof(out->entry_index));
                out->entry_index[sizeof(out->entry_index) - 1] = '\0';
                out->entry_subindex = entry->subindex;
                out->is_output = is_output;
                out->bit_offset = bit;
                out->bit_length = entry->bit_length;
                out->data_type = entry->parsed_type;
                memcpy(out->name, entry->name, sizeof(out->name));
                out->name[sizeof(out->name) - 1] = '\0';
            }
            bit += entry->bit_length;
        }
    }
    return 0;
}

int ecat_layout_build(ecat_master_instance_t *inst, edog_logger_t *logger)
{
    ecat_layout_t *layout = &inst->layout;
    memset(layout, 0, sizeof(*layout));

    if (!inst->soem_initialized) {
        edog_log_error(logger, "Master '%s': layout requested before mapping", inst->name);
        return -1;
    }

    const ec_groupt *grp = &inst->ecx_context.grouplist[0];
    layout->output_bytes = grp->Obytes;
    layout->input_bytes = grp->Ibytes;

    if (layout->output_bytes > ECAT_MAX_FRAME_PAYLOAD ||
        layout->input_bytes > ECAT_MAX_FRAME_PAYLOAD) {
        edog_log_error(logger,
                       "Master '%s': process image (out=%u in=%u bytes) exceeds the %d-byte "
                       "frame payload",
                       inst->name, layout->output_bytes, layout->input_bytes,
                       ECAT_MAX_FRAME_PAYLOAD);
        return -1;
    }

    for (int s = 0; s < inst->config.slave_count; s++) {
        const ecat_slave_t *cfg = &inst->config.slaves[s];
        const ec_slavet *soem = ecat_master_get_slave(inst, cfg->position);
        if (soem == NULL) {
            edog_log_error(logger, "Master '%s': slave %d (%s) not found on the bus", inst->name,
                           cfg->position, cfg->name);
            return -1;
        }
        if (add_direction(inst, cfg, soem, true, logger) != 0 ||
            add_direction(inst, cfg, soem, false, logger) != 0)
            return -1;
    }

    edog_log_info(logger, "Master '%s': layout built: %d entries, outputs=%u bytes, inputs=%u bytes",
                  inst->name, layout->entry_count, layout->output_bytes, layout->input_bytes);
    return 0;
}
