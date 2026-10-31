#include "bl616_bridge_mapping.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define BUTTON(button) BL616_BRIDGE_BUTTON_MASK(button)

static bl616_bridge_input_state_t make_input(void)
{
    bl616_bridge_input_state_t input;

    bl616_bridge_input_state_reset(&input);
    input.valid = 1U;
    input.source = BL616_BRIDGE_INPUT_SOURCE_DUALSENSE_BT;
    input.buttons = BUTTON(BL616_BRIDGE_BUTTON_EAST);
    input.left_x = 1234;
    input.left_y = -2345;
    input.right_x = 3000;
    input.right_y = -4000;
    input.accel[0] = 11;
    input.gyro[2] = -22;
    input.motion_valid = 1U;
    input.sensor_timestamp = UINT32_C(0x12345678);
    input.timestamp_valid = 1U;
    return input;
}

static void test_identity_and_remap(void)
{
    bl616_bridge_mapping_config_t mapping;
    bl616_bridge_input_state_t input = make_input();
    bl616_bridge_input_state_t output;

    bl616_bridge_mapping_defaults(&mapping);
    assert(bl616_bridge_mapping_validate(&mapping));
    assert(bl616_bridge_mapping_is_identity(&mapping));
    assert(bl616_bridge_mapping_apply(&mapping, &input, &output));
    assert(memcmp(&input, &output, sizeof(input)) == 0);

    assert(bl616_bridge_mapping_set(
        &mapping, BL616_BRIDGE_BUTTON_SOUTH,
        BL616_BRIDGE_BUTTON_EAST));
    assert(bl616_bridge_mapping_set(
        &mapping, BL616_BRIDGE_BUTTON_EAST,
        BL616_BRIDGE_MAPPING_NONE));
    assert(bl616_bridge_mapping_set(
        &mapping, BL616_BRIDGE_BUTTON_NORTH,
        BL616_BRIDGE_BUTTON_EAST));
    assert(!bl616_bridge_mapping_is_identity(&mapping));
    assert(bl616_bridge_mapping_apply(&mapping, &input, &output));
    assert((output.buttons & BUTTON(BL616_BRIDGE_BUTTON_SOUTH)) != 0U);
    assert((output.buttons & BUTTON(BL616_BRIDGE_BUTTON_NORTH)) != 0U);
    assert((output.buttons & BUTTON(BL616_BRIDGE_BUTTON_EAST)) == 0U);
    assert(output.left_x == input.left_x && output.gyro[2] == input.gyro[2]);
}

static void test_trigger_targets(void)
{
    bl616_bridge_mapping_config_t mapping;
    bl616_bridge_input_state_t input = make_input();
    bl616_bridge_input_state_t output;

    bl616_bridge_mapping_defaults(&mapping);
    input.buttons = BUTTON(BL616_BRIDGE_BUTTON_SOUTH);
    input.left_trigger = 123U;
    input.right_trigger = 456U;
    assert(bl616_bridge_mapping_set(
        &mapping, BL616_BRIDGE_BUTTON_LEFT_TRIGGER,
        BL616_BRIDGE_BUTTON_SOUTH));
    assert(bl616_bridge_mapping_set(
        &mapping, BL616_BRIDGE_BUTTON_RIGHT_TRIGGER,
        BL616_BRIDGE_MAPPING_NONE));
    assert(bl616_bridge_mapping_apply(&mapping, &input, &output));
    assert(output.left_trigger == UINT16_MAX);
    assert(output.right_trigger == 0U);
    assert((output.buttons & BUTTON(BL616_BRIDGE_BUTTON_LEFT_TRIGGER)) != 0U);
    assert((output.buttons & BUTTON(BL616_BRIDGE_BUTTON_RIGHT_TRIGGER)) == 0U);
}

static void test_wire_and_commands(void)
{
    bl616_bridge_mapping_config_t mapping;
    bl616_bridge_mapping_config_t decoded;
    bl616_bridge_mapping_profiles_t profiles;
    bl616_bridge_mapping_profiles_t decoded_profiles;
    bl616_bridge_mapping_command_t command;
    uint8_t legacy_wire[BL616_BRIDGE_MAPPING_CONFIG_WIRE_SIZE];
    uint8_t profiles_wire[BL616_BRIDGE_MAPPING_WIRE_SIZE];
    char json[1200];

    bl616_bridge_mapping_defaults(&mapping);
    assert(bl616_bridge_mapping_set(
        &mapping, BL616_BRIDGE_BUTTON_SOUTH,
        BL616_BRIDGE_BUTTON_EAST));
    assert(bl616_bridge_mapping_set(
        &mapping, BL616_BRIDGE_BUTTON_EAST,
        BL616_BRIDGE_MAPPING_NONE));
    assert(bl616_bridge_mapping_serialize(
               &mapping, legacy_wire, sizeof(legacy_wire)) ==
           sizeof(legacy_wire));
    assert(bl616_bridge_mapping_deserialize(
        legacy_wire, sizeof(legacy_wire), &decoded));
    assert(memcmp(&mapping, &decoded, sizeof(mapping)) == 0);
    legacy_wire[8] ^= 1U;
    assert(!bl616_bridge_mapping_deserialize(
        legacy_wire, sizeof(legacy_wire), &decoded));

    bl616_bridge_mapping_profiles_defaults(&profiles);
    assert(bl616_bridge_mapping_set(
        &profiles.ds5, BL616_BRIDGE_BUTTON_SOUTH,
        BL616_BRIDGE_BUTTON_EAST));
    assert(bl616_bridge_mapping_set(
        &profiles.ns2pro, BL616_BRIDGE_BUTTON_NORTH,
        BL616_BRIDGE_BUTTON_WEST));
    assert(bl616_bridge_mapping_profiles_serialize(
               &profiles, profiles_wire, sizeof(profiles_wire)) ==
           sizeof(profiles_wire));
    assert(bl616_bridge_mapping_profiles_deserialize(
        profiles_wire, sizeof(profiles_wire), &decoded_profiles));
    assert(memcmp(&profiles, &decoded_profiles, sizeof(profiles)) == 0);
    assert(decoded_profiles.ds5.source_for_target[
               BL616_BRIDGE_BUTTON_SOUTH] ==
           BL616_BRIDGE_BUTTON_EAST);
    assert(decoded_profiles.ns2pro.source_for_target[
               BL616_BRIDGE_BUTTON_NORTH] ==
           BL616_BRIDGE_BUTTON_WEST);
    profiles_wire[48] ^= 1U;
    assert(!bl616_bridge_mapping_profiles_deserialize(
        profiles_wire, sizeof(profiles_wire), &decoded_profiles));

    assert(bl616_bridge_mapping_parse_command("mapping get", &command) == 1);
    assert(command.kind == BL616_BRIDGE_MAPPING_COMMAND_GET);
    assert(command.profile == BL616_BRIDGE_MAPPING_PROFILE_UNSPECIFIED);
    assert(bl616_bridge_mapping_parse_command(
        "mapping get ds5", &command) == 1);
    assert(command.profile == BL616_BRIDGE_MAPPING_PROFILE_DS5);
    assert(bl616_bridge_mapping_parse_command(
        "mapping get ns2pro", &command) == 1);
    assert(command.profile == BL616_BRIDGE_MAPPING_PROFILE_NS2PRO);
    assert(bl616_bridge_mapping_parse_command(
        "mapping set south east", &command) == 1);
    assert(command.kind == BL616_BRIDGE_MAPPING_COMMAND_SET);
    assert(command.target == BL616_BRIDGE_BUTTON_SOUTH);
    assert(command.source == BL616_BRIDGE_BUTTON_EAST);
    assert(bl616_bridge_mapping_parse_command(
        "mapping set ds5 south east", &command) == 1);
    assert(command.profile == BL616_BRIDGE_MAPPING_PROFILE_DS5);
    assert(bl616_bridge_mapping_parse_command(
        "mapping set ns2pro north west", &command) == 1);
    assert(command.profile == BL616_BRIDGE_MAPPING_PROFILE_NS2PRO);
    assert(bl616_bridge_mapping_parse_command(
        "mapping set east none", &command) == 1);
    assert(command.source == BL616_BRIDGE_MAPPING_NONE);
    assert(bl616_bridge_mapping_parse_command("mapping reset", &command) == 1);
    assert(bl616_bridge_mapping_parse_command("mapping save", &command) == 1);
    assert(bl616_bridge_mapping_parse_command(
        "mapping reset ds5", &command) == 1);
    assert(command.profile == BL616_BRIDGE_MAPPING_PROFILE_DS5);
    assert(bl616_bridge_mapping_parse_command(
        "mapping save ns2pro", &command) == 1);
    assert(command.profile == BL616_BRIDGE_MAPPING_PROFILE_NS2PRO);
    assert(bl616_bridge_mapping_parse_command("bridge status", &command) == 0);
    assert(bl616_bridge_mapping_parse_command(
        "mapping set SOUTH east", &command) == -1);
    assert(bl616_bridge_mapping_parse_command(
        "mapping set south east extra", &command) == -1);

    assert(bl616_bridge_mapping_format_json(
        &mapping, true, json, sizeof(json)) > 0);
    assert(strstr(json, "\"identity\":false") != 0);
    assert(strstr(json, "\"dirty\":true") != 0);
    assert(strstr(json, "\"south\":\"east\"") != 0);
    assert(strstr(json, "\"east\":\"none\"") != 0);
    assert(bl616_bridge_mapping_format_profile_json(
               BL616_BRIDGE_MAPPING_PROFILE_DS5, &mapping, true,
               json, sizeof(json)) > 0);
    assert(strstr(json, "\"profile\":\"ds5\"") != 0);
    assert(bl616_bridge_mapping_format_profile_json(
               BL616_BRIDGE_MAPPING_PROFILE_NS2PRO,
               &profiles.ns2pro, false, json, sizeof(json)) > 0);
    assert(strstr(json, "\"profile\":\"ns2pro\"") != 0);
}

static void assert_preserved_except(const uint8_t *before,
                                    const uint8_t *after,
                                    size_t len,
                                    const uint8_t *changed,
                                    size_t changed_count)
{
    size_t index;

    for (index = 0U; index < len; ++index) {
        size_t item;
        int may_change = 0;

        for (item = 0U; item < changed_count; ++item) {
            if (index == changed[item]) {
                may_change = 1;
            }
        }
        if (!may_change) {
            assert(before[index] == after[index]);
        }
    }
}

static void test_native_report_patching(void)
{
    static const uint8_t ds5_changed[] = {5U, 6U, 8U, 9U, 10U};
    static const uint8_t ns2_changed[] = {5U, 6U, 7U, 8U};
    bl616_bridge_mapping_config_t mapping;
    bl616_bridge_input_state_t input = make_input();
    uint8_t report[64];
    uint8_t before[64];

    bl616_bridge_mapping_defaults(&mapping);
    memset(report, 0x3c, sizeof(report));
    report[0] = 0x01U;
    memcpy(before, report, sizeof(report));
    assert(bl616_bridge_mapping_patch_native_report(
        &mapping, BL616_BRIDGE_ROLE_DUALSENSE,
        &input, report, sizeof(report)));
    assert(memcmp(before, report, sizeof(report)) == 0);

    assert(bl616_bridge_mapping_set(
        &mapping, BL616_BRIDGE_BUTTON_SOUTH,
        BL616_BRIDGE_BUTTON_EAST));
    assert(bl616_bridge_mapping_set(
        &mapping, BL616_BRIDGE_BUTTON_EAST,
        BL616_BRIDGE_MAPPING_NONE));

    memset(report, 0xa5, sizeof(report));
    report[0] = 0x01U;
    report[5] = 0U;
    report[6] = 0U;
    report[8] = 0x48U; /* neutral d-pad + east */
    report[9] = 0U;
    report[10] = 0x08U; /* reserved bit must survive */
    memcpy(before, report, sizeof(report));
    assert(bl616_bridge_mapping_patch_native_report(
        &mapping, BL616_BRIDGE_ROLE_DUALSENSE,
        &input, report, sizeof(report)));
    assert(report[8] == 0x28U); /* neutral d-pad + mapped south */
    assert(report[10] == 0x08U);
    assert_preserved_except(before, report, sizeof(report),
                            ds5_changed, sizeof(ds5_changed));

    memset(report, 0x5a, sizeof(report));
    report[0] = 0x05U;
    report[5] = 0x38U; /* east + reserved bits */
    report[6] = 0x80U;
    report[7] = 0x30U;
    report[8] = 0xfcU;
    memcpy(before, report, sizeof(report));
    assert(bl616_bridge_mapping_patch_native_report(
        &mapping, BL616_BRIDGE_ROLE_NS2PRO,
        &input, report, sizeof(report)));
    assert(report[5] == 0x34U); /* mapped south + reserved bits */
    assert(report[6] == 0x80U && report[7] == 0x30U && report[8] == 0xfcU);
    assert_preserved_except(before, report, sizeof(report),
                            ns2_changed, sizeof(ns2_changed));
}

static void repair_wire_crc(uint8_t *wire, size_t len)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    unsigned int bit;

    for (i = 0U; i < len - 4U; ++i) {
        crc ^= wire[i];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
        }
    }
    crc = ~crc;
    for (i = 0U; i < 4U; ++i) {
        wire[len - 4U + i] = (uint8_t)(crc >> (8U * i));
    }
}

static void test_wire_validation_is_atomic(void)
{
    static const size_t invalid_fields[] = {0U, 4U, 5U, 6U, 7U, 8U, 48U};
    bl616_bridge_mapping_profiles_t profiles;
    bl616_bridge_mapping_profiles_t decoded;
    bl616_bridge_mapping_profiles_t sentinel;
    bl616_bridge_mapping_config_t legacy;
    bl616_bridge_mapping_config_t legacy_before;
    uint8_t wire[BL616_BRIDGE_MAPPING_WIRE_SIZE + 1U];
    uint8_t good[BL616_BRIDGE_MAPPING_WIRE_SIZE];
    uint8_t legacy_wire[BL616_BRIDGE_MAPPING_CONFIG_WIRE_SIZE];
    size_t i;

    bl616_bridge_mapping_profiles_defaults(&profiles);
    profiles.ds5.source_for_target[0] = BL616_BRIDGE_MAPPING_NONE;
    profiles.ns2pro.source_for_target[1] = BL616_BRIDGE_BUTTON_C;
    assert(bl616_bridge_mapping_profiles_serialize(
        &profiles, good, sizeof(good)) == sizeof(good));
    memset(&sentinel, 0xa5, sizeof(sentinel));
    for (i = 0U; i < BL616_BRIDGE_MAPPING_WIRE_SIZE; ++i) {
        memset(wire, 0x5a, sizeof(wire));
        assert(bl616_bridge_mapping_profiles_serialize(
            &profiles, wire, i) == 0U);
        assert(wire[0] == 0x5aU && wire[sizeof(wire) - 1U] == 0x5aU);
        decoded = sentinel;
        assert(!bl616_bridge_mapping_profiles_deserialize(good, i, &decoded));
        assert(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
    }
    memcpy(wire, good, sizeof(good));
    wire[sizeof(good)] = 0U;
    decoded = sentinel;
    assert(!bl616_bridge_mapping_profiles_deserialize(
        wire, sizeof(wire), &decoded));
    assert(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);

    for (i = 0U; i < sizeof(invalid_fields) / sizeof(invalid_fields[0]); ++i) {
        memcpy(wire, good, sizeof(good));
        wire[invalid_fields[i]] = 0xfeU;
        repair_wire_crc(wire, sizeof(good));
        decoded = sentinel;
        assert(!bl616_bridge_mapping_profiles_deserialize(
            wire, sizeof(good), &decoded));
        assert(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
    }
    assert(bl616_bridge_mapping_serialize(
        &profiles.ds5, legacy_wire, sizeof(legacy_wire)) == sizeof(legacy_wire));
    legacy_wire[8] = BL616_BRIDGE_BUTTON_COUNT;
    repair_wire_crc(legacy_wire, sizeof(legacy_wire));
    legacy_before = profiles.ns2pro;
    legacy = legacy_before;
    assert(!bl616_bridge_mapping_deserialize(
        legacy_wire, sizeof(legacy_wire), &legacy));
    assert(memcmp(&legacy, &legacy_before, sizeof(legacy)) == 0);

    for (i = 0U; i < 2U; ++i) {
        bl616_bridge_mapping_profiles_t invalid = profiles;
        bl616_bridge_mapping_config_t *config =
            i == 0U ? &invalid.ds5 : &invalid.ns2pro;
        config->source_for_target[BL616_BRIDGE_BUTTON_C] =
            BL616_BRIDGE_BUTTON_COUNT;
        memset(wire, 0xa5, sizeof(wire));
        assert(!bl616_bridge_mapping_profiles_validate(&invalid));
        assert(bl616_bridge_mapping_profiles_serialize(
            &invalid, wire, sizeof(wire)) == 0U);
        assert(wire[0] == 0xa5U && wire[sizeof(wire) - 1U] == 0xa5U);
    }
    assert(!bl616_bridge_mapping_profiles_deserialize(
        NULL, sizeof(good), &decoded));
    assert(!bl616_bridge_mapping_profiles_deserialize(
        good, sizeof(good), NULL));
}

int main(void)
{
    test_identity_and_remap();
    test_trigger_targets();
    test_wire_and_commands();
    test_native_report_patching();
    test_wire_validation_is_atomic();
    return 0;
}
