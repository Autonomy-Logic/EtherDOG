// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_busconfig_contract.c
 * @brief Busconfig fields for modular couplers, mandatory and fixed PDOs, and strict hex ids.
 *
 * Every refusal is checked through ecat_config_parse_all: one invalid master refuses the
 * whole configuration.
 */

#include "config.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TEMP_FILE = "test_busconfig_contract_tmp.json";
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
    remove(TEMP_FILE);
}

/* One slave; @p identity, @p extra and the PDO arrays are raw JSON fragments. */
static int load(const char *identity, const char *extra, const char *rx, const char *tx)
{
    FILE *fp = fopen(TEMP_FILE, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fprintf(fp,
            "[{\"name\":\"m\",\"protocol\":\"ETHERCAT\",\"config\":{"
            "\"master\":{\"interface\":\"eth0\",\"cycle_time_us\":1000,\"receive_timeout_us\":2000},"
            "\"slaves\":[{\"position\":1,\"name\":\"s\",%s%s,"
            "\"rx_pdos\":%s,\"tx_pdos\":%s}]}}]",
            identity, extra, rx, tx);
    fclose(fp);
    return ecat_config_parse_all(TEMP_FILE, g, ECAT_MAX_MASTERS, &g_count);
}

#define IDENTITY "\"vendor_id\":\"0x00000230\",\"product_code\":\"0x00000001\""
#define NO_PDOS "[]"

static const char *modular(const char *slots, const char *modules)
{
    static char buf[1024];
    snprintf(buf, sizeof(buf), ",\"modular\":{\"slot_index_increment\":16,\"slots\":%s,\"modules\":%s}",
             slots, modules);
    return buf;
}

#define DI(slot) "{\"slot\":" #slot ",\"ident\":\"0x00091F84\",\"class\":\"Di\"}"
#define AI(slot) "{\"slot\":" #slot ",\"ident\":\"0x040115C4\",\"class\":\"Ai\"}"

/* --- modular ----------------------------------------------------------------------------- */

void test_modular_ParsesSlotRulesAndModules(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
        load(IDENTITY,
             modular("[{\"min_instances\":0,\"max_instances\":4,\"module_classes\":[\"Di\",\"Ai\"],"
                     "\"module_idents\":[\"0x00000001\"]}]",
                     "[" DI(1) "," AI(2) "]"),
             NO_PDOS, NO_PDOS));
    const ecat_modular_t *m = &g[0].config.slaves[0].modular;
    TEST_ASSERT_TRUE(m->present);
    TEST_ASSERT_EQUAL_INT(16, m->slot_index_increment);
    TEST_ASSERT_EQUAL_INT(1, m->slot_count);
    TEST_ASSERT_EQUAL_INT(2, m->slots[0].class_count);
    TEST_ASSERT_EQUAL_STRING("Ai", m->slots[0].classes[1]);
    TEST_ASSERT_EQUAL_INT(1, m->slots[0].ident_count);
    TEST_ASSERT_EQUAL_HEX32(1, m->slots[0].idents[0]);
    TEST_ASSERT_EQUAL_INT(2, m->module_count);
    TEST_ASSERT_EQUAL_INT(2, m->modules[1].slot);
    TEST_ASSERT_EQUAL_HEX32(0x040115C4, m->modules[1].ident);
    TEST_ASSERT_EQUAL_STRING("Ai", m->modules[1].class_name);
}

void test_modular_AbsentSectionIsNotModular(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, load(IDENTITY, "", NO_PDOS, NO_PDOS));
    TEST_ASSERT_FALSE(g[0].config.slaves[0].modular.present);
}

void test_modular_ClassNotAcceptedBySlot_IsRefused(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
        load(IDENTITY,
             modular("[{\"min_instances\":0,\"max_instances\":4,\"module_classes\":[\"Di\"]}]",
                     "[" AI(1) "]"),
             NO_PDOS, NO_PDOS));
}

void test_modular_IdentAcceptedWithoutClass(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
        load(IDENTITY,
             modular("[{\"min_instances\":0,\"max_instances\":1,\"module_idents\":[\"0x00091F84\"]}]",
                     "[{\"slot\":1,\"ident\":\"0x00091f84\"}]"),
             NO_PDOS, NO_PDOS));
}

void test_modular_MoreThanMaxInstances_IsRefused(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
        load(IDENTITY,
             modular("[{\"min_instances\":0,\"max_instances\":1,\"module_classes\":[\"Di\"]}]",
                     "[" DI(1) "," DI(2) "]"),
             NO_PDOS, NO_PDOS));
}

void test_modular_SlotsFillDefinitionsInOrder(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
        load(IDENTITY,
             modular("[{\"min_instances\":1,\"max_instances\":2,\"module_classes\":[\"Di\"]},"
                     "{\"min_instances\":0,\"max_instances\":2,\"module_classes\":[\"Ai\"]}]",
                     "[" DI(1) "," AI(2) "," AI(3) "]"),
             NO_PDOS, NO_PDOS));
}

void test_modular_MinInstancesUnmet_IsRefused(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
        load(IDENTITY,
             modular("[{\"min_instances\":0,\"max_instances\":1,\"module_classes\":[\"Di\"]},"
                     "{\"min_instances\":1,\"max_instances\":1,\"module_classes\":[\"Ai\"]}]",
                     "[" DI(1) "]"),
             NO_PDOS, NO_PDOS));
}

void test_modular_MinInstancesUnmetWithoutModules_IsRefused(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
        load(IDENTITY,
             modular("[{\"min_instances\":1,\"max_instances\":1,\"module_classes\":[\"Di\"]}]", "[]"),
             NO_PDOS, NO_PDOS));
}

void test_modular_ZeroIncrementIsAccepted(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
        load(IDENTITY, ",\"modular\":{\"slot_index_increment\":0,\"slots\":[],\"modules\":[]}",
             NO_PDOS, NO_PDOS));
    TEST_ASSERT_EQUAL_INT(0, g[0].config.slaves[0].modular.slot_index_increment);
}

void test_modular_NoRulesAndNoModules_IsAccepted(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, load(IDENTITY, modular("[]", "[]"), NO_PDOS, NO_PDOS));
    TEST_ASSERT_TRUE(g[0].config.slaves[0].modular.present);
}

void test_modular_EmptySlotCountsTowardsDefinition(void)
{
    /* Slot 1 empty uses the only instance of definition 1, so the Di in slot 2 moves on. */
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
        load(IDENTITY,
             modular("[{\"min_instances\":0,\"max_instances\":1,\"module_classes\":[\"Di\"]}]",
                     "[" DI(2) "]"),
             NO_PDOS, NO_PDOS));
}

void test_modular_SlotsOutOfOrderOrRepeated_AreRefused(void)
{
    const char *rules = "[{\"min_instances\":0,\"max_instances\":8,\"module_classes\":[\"Di\"]}]";
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
                          load(IDENTITY, modular(rules, "[" DI(2) "," DI(1) "]"), NO_PDOS, NO_PDOS));
    for (int i = 0; i < ECAT_MAX_MASTERS; i++)
        ecat_config_destroy(&g[i].config);
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
                          load(IDENTITY, modular(rules, "[" DI(1) "," DI(1) "]"), NO_PDOS, NO_PDOS));
}

void test_modular_ModulesWithoutSlotRules_AreRefused(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
                          load(IDENTITY, modular("[]", "[" DI(1) "]"), NO_PDOS, NO_PDOS));
}

void test_modular_BadIncrementSlotOrIdent_AreRefused(void)
{
    const char *bad[] = {
        ",\"modular\":{\"slot_index_increment\":-1,\"slots\":[],\"modules\":[]}",
        ",\"modular\":{\"slot_index_increment\":4096,\"slots\":[],\"modules\":[]}",
        ",\"modular\":{\"slot_index_increment\":16,\"slots\":[{\"min_instances\":0,\"max_instances\":2}],"
        "\"modules\":[{\"slot\":0,\"ident\":\"0x1\"}]}",
        ",\"modular\":{\"slot_index_increment\":16,\"slots\":[{\"min_instances\":0,\"max_instances\":2}],"
        "\"modules\":[{\"slot\":1,\"ident\":\"12\"}]}",
        ",\"modular\":{\"slot_index_increment\":16,\"slots\":[{\"min_instances\":2,\"max_instances\":1}],"
        "\"modules\":[]}",
        ",\"modular\":[]",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ECAT_CONFIG_ERR_INVALID, load(IDENTITY, bad[i], NO_PDOS, NO_PDOS),
                                      bad[i]);
        for (int k = 0; k < ECAT_MAX_MASTERS; k++)
            ecat_config_destroy(&g[k].config);
    }
}

/* --- PDOs -------------------------------------------------------------------------------- */

#define PDO(idx, fixed) \
    "{\"index\":\"" idx "\",\"name\":\"p\",\"fixed\":" fixed ",\"entries\":[" \
    "{\"index\":\"0x6000\",\"subindex\":1,\"bit_length\":8,\"data_type\":\"UINT8\",\"name\":\"e\"}]}"

void test_pdos_FixedFlagIsParsed(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
                          load(IDENTITY, "", "[" PDO("0x1600", "false") "]", "[" PDO("0x1A00", "true") "]"));
    TEST_ASSERT_FALSE(g[0].config.slaves[0].rx_pdos[0].fixed);
    TEST_ASSERT_TRUE(g[0].config.slaves[0].tx_pdos[0].fixed);
}

void test_pdos_SameIndexTwice_IsRefused(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
                          load(IDENTITY, "", "[" PDO("0x1600", "false") "]", "[" PDO("0x1600", "false") "]"));
}

void test_pdos_MandatoryAssigned_IsAccepted(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
                          load(IDENTITY, ",\"mandatory_pdos\":[\"0x1a00\"]", NO_PDOS,
                               "[" PDO("0x1A00", "true") "]"));
    TEST_ASSERT_EQUAL_INT(1, g[0].config.slaves[0].mandatory_pdo_count);
    TEST_ASSERT_EQUAL_STRING("0x1A00", g[0].config.slaves[0].mandatory_pdos[0]);
}

void test_pdos_MandatoryNotAssigned_IsRefused(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
                          load(IDENTITY, ",\"mandatory_pdos\":[\"0x1A01\"]", NO_PDOS,
                               "[" PDO("0x1A00", "true") "]"));
}

void test_pdos_MandatoryNotHex_IsRefused(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID,
                          load(IDENTITY, ",\"mandatory_pdos\":[\"1A00\"]", NO_PDOS, NO_PDOS));
}

/* --- identity ---------------------------------------------------------------------------- */

void test_identity_HexForms(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
                          load("\"vendor_id\":\"#x2\",\"product_code\":\"0X0A\"", "", NO_PDOS, NO_PDOS));
    TEST_ASSERT_EQUAL_HEX32(2, g[0].config.slaves[0].vendor_id);
    TEST_ASSERT_EQUAL_HEX32(0x0A, g[0].config.slaves[0].product_code);
}

void test_identity_MalformedOrMissing_IsRefused(void)
{
    const char *bad[] = {
        "\"vendor_id\":\"2\",\"product_code\":\"0x1\"",
        "\"vendor_id\":\"0xZZ\",\"product_code\":\"0x1\"",
        "\"vendor_id\":\"0x123456789\",\"product_code\":\"0x1\"",
        "\"vendor_id\":\"0x2\"",
        "\"vendor_id\":2,\"product_code\":\"0x1\"",
        "\"vendor_id\":\"0x2\",\"product_code\":\"0x1\",\"revision\":\"x1\"",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ECAT_CONFIG_ERR_INVALID, load(bad[i], "", NO_PDOS, NO_PDOS), bad[i]);
        for (int k = 0; k < ECAT_MAX_MASTERS; k++)
            ecat_config_destroy(&g[k].config);
    }
}

static int load_bytes_sdo(const char *payload)
{
    char extra[400];
    snprintf(extra, sizeof(extra),
             ",\"sdo_configurations\":[{\"index\":\"0xF030\",\"subindex\":0,\"data_type\":\"OCTET_STRING\","
             "\"value_bytes\":\"%s\",\"complete_access\":true,\"name\":\"list\"}]",
             payload);
    return load(IDENTITY, extra, NO_PDOS, NO_PDOS);
}

void test_sdo_ValueBytesHexPairs_AreDecoded(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, load_bytes_sdo("0x0100841F0900"));
    const ecat_sdo_config_t *sdo = &g[0].config.slaves[0].sdo_configs[0];
    TEST_ASSERT_EQUAL_INT(6, (int)sdo->value_bytes_len);
    TEST_ASSERT_EQUAL_HEX8(0x84, sdo->value_bytes[2]);
    TEST_ASSERT_TRUE(sdo->complete_access);
}

void test_sdo_ValueBytesWithNonHexPair_IsRefused(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_ERR_INVALID, load_bytes_sdo("0x01zz"));
}
