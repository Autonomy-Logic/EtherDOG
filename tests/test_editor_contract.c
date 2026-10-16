// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_editor_contract.c
 * @brief The OpenPLC Editor's generated busconfigs (tests/fixtures/editor) load as the editor meant.
 */

#include "config.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static ecat_master_instance_t *g;
static int g_count;

void setUp(void)
{
    g = calloc(ECAT_MAX_MASTERS, sizeof(*g));
    TEST_ASSERT_NOT_NULL(g);
    g_count = 0;
}

void tearDown(void)
{
    for (int i = 0; i < ECAT_MAX_MASTERS; i++)
        ecat_config_destroy(&g[i].config);
    free(g);
}

static const ecat_slave_t *load_first_slave(const char *file)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/editor/%s", EDOG_TEST_FIXTURES, file);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ECAT_CONFIG_OK, ecat_config_parse_all(path, g, ECAT_MAX_MASTERS, &g_count),
                                  path);
    TEST_ASSERT_EQUAL_INT(1, g_count);
    TEST_ASSERT_EQUAL_STRING("bus_a", g[0].name);
    TEST_ASSERT_EQUAL_INT(1, g[0].config.slave_count);
    return &g[0].config.slaves[0];
}

static const ecat_sdo_config_t *find_sdo(const ecat_slave_t *s, const char *index, int subindex)
{
    for (int i = 0; i < s->sdo_count; i++) {
        if (strcasecmp(s->sdo_configs[i].index, index) == 0 && s->sdo_configs[i].subindex == subindex)
            return &s->sdo_configs[i];
    }
    return NULL;
}

void test_ur20_modular_station_loads_with_its_slot_rules_and_modules(void)
{
    const ecat_slave_t *s = load_first_slave("ur20-modular.busconfig.golden.json");
    TEST_ASSERT_EQUAL_HEX32(0x230, s->vendor_id);
    TEST_ASSERT_EQUAL_HEX32(0x4F911C30, s->product_code);

    const ecat_modular_t *m = &s->modular;
    TEST_ASSERT_TRUE(m->present);
    TEST_ASSERT_EQUAL_INT(16, m->slot_index_increment);
    TEST_ASSERT_EQUAL_INT(1, m->slot_count);
    TEST_ASSERT_EQUAL_INT(1, m->slots[0].min_instances);
    TEST_ASSERT_EQUAL_INT(64, m->slots[0].max_instances);
    TEST_ASSERT_EQUAL_INT(3, m->module_count);
    int slots[] = { 1, 2, 4 };
    uint32_t idents[] = { 0x00091F84, 0x01012FA0, 0x040115C4 };
    const char *classes[] = { "Di", "Do", "Ai" };
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(slots[i], m->modules[i].slot);
        TEST_ASSERT_EQUAL_HEX32(idents[i], m->modules[i].ident);
        TEST_ASSERT_EQUAL_STRING(classes[i], m->modules[i].class_name);
    }
}

void test_ur20_pdos_are_fixed_mandatory_and_unique(void)
{
    const ecat_slave_t *s = load_first_slave("ur20-modular.busconfig.golden.json");
    TEST_ASSERT_EQUAL_INT(2, s->rx_pdo_count);
    TEST_ASSERT_EQUAL_INT(4, s->tx_pdo_count);
    for (int i = 0; i < s->rx_pdo_count; i++)
        TEST_ASSERT_TRUE(s->rx_pdos[i].fixed);
    for (int i = 0; i < s->tx_pdo_count; i++)
        TEST_ASSERT_TRUE(s->tx_pdos[i].fixed);
    TEST_ASSERT_EQUAL_INT(16, s->rx_pdos[0].entry_count);
    TEST_ASSERT_EQUAL_INT(6, s->mandatory_pdo_count);
    TEST_ASSERT_EQUAL_STRING("0x16FF", s->mandatory_pdos[0]);
    TEST_ASSERT_EQUAL_STRING("0x1A03", s->mandatory_pdos[5]);
}

void test_ur20_module_list_is_one_complete_access_write(void)
{
    const ecat_slave_t *s = load_first_slave("ur20-modular.busconfig.golden.json");
    const ecat_sdo_config_t *list = find_sdo(s, "0xF030", 0);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_TRUE(list->complete_access);
    /* Count 4 padded to 16 bits, then one UDINT per slot, slot 3 empty. */
    static const uint8_t want[] = { 0x04, 0x00, 0x84, 0x1F, 0x09, 0x00, 0xA0, 0x2F, 0x01,
                                    0x01, 0x00, 0x00, 0x00, 0x00, 0xC4, 0x15, 0x01, 0x04 };
    TEST_ASSERT_EQUAL_INT((int)sizeof(want), (int)list->value_bytes_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, list->value_bytes, sizeof(want));
    /* The list is the last startup SDO. */
    TEST_ASSERT_EQUAL_PTR(&s->sdo_configs[s->sdo_count - 1], list);
}

void test_ur20_module_names_are_byte_strings_not_ascii_hex(void)
{
    const ecat_slave_t *s = load_first_slave("ur20-modular.busconfig.golden.json");
    const ecat_sdo_config_t *name = find_sdo(s, "0x8000", 3);
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_INT(10, (int)name->value_bytes_len);
    TEST_ASSERT_EQUAL_MEMORY("UR20-4DI-P", name->value_bytes, 10);
    TEST_ASSERT_FALSE(name->complete_access);
}

void test_delta_plain_slave_loads_without_modular_or_mandatory(void)
{
    const ecat_slave_t *s = load_first_slave("delta-multimode.busconfig.golden.json");
    TEST_ASSERT_FALSE(s->modular.present);
    TEST_ASSERT_EQUAL_INT(0, s->mandatory_pdo_count);
    TEST_ASSERT_EQUAL_INT(1, s->rx_pdo_count);
    TEST_ASSERT_FALSE(s->rx_pdos[0].fixed);

    /* PDO assignment: clear, entry, count, numeric. */
    const ecat_sdo_config_t *entry = find_sdo(s, "0x1C12", 1);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_NULL(entry->value_bytes);
    TEST_ASSERT_EQUAL_INT(0x1600, (int)entry->value);
    TEST_ASSERT_EQUAL_INT(ECAT_DTYPE_UINT16, entry->parsed_type);
}
