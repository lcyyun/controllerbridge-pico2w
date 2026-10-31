#ifndef BL616_BRIDGE_MAPPING_H
#define BL616_BRIDGE_MAPPING_H

#include "bl616_bridge_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BL616_BRIDGE_MAPPING_NONE UINT8_C(0xff)
#define BL616_BRIDGE_MAPPING_CONFIG_WIRE_SIZE 40U
#define BL616_BRIDGE_MAPPING_WIRE_SIZE 80U

typedef struct {
    uint8_t source_for_target[BL616_BRIDGE_BUTTON_COUNT];
} bl616_bridge_mapping_config_t;

/* Profiles follow the physical input source, never the emulated USB role. */
typedef enum {
    BL616_BRIDGE_MAPPING_PROFILE_UNSPECIFIED = 0,
    BL616_BRIDGE_MAPPING_PROFILE_DS5,
    BL616_BRIDGE_MAPPING_PROFILE_NS2PRO
} bl616_bridge_mapping_profile_t;

typedef struct {
    bl616_bridge_mapping_config_t ds5;
    bl616_bridge_mapping_config_t ns2pro;
} bl616_bridge_mapping_profiles_t;

typedef enum {
    BL616_BRIDGE_MAPPING_COMMAND_NONE = 0,
    BL616_BRIDGE_MAPPING_COMMAND_GET,
    BL616_BRIDGE_MAPPING_COMMAND_SET,
    BL616_BRIDGE_MAPPING_COMMAND_RESET,
    BL616_BRIDGE_MAPPING_COMMAND_SAVE
} bl616_bridge_mapping_command_kind_t;

typedef struct {
    bl616_bridge_mapping_command_kind_t kind;
    bl616_bridge_mapping_profile_t profile;
    bl616_bridge_button_t target;
    uint8_t source;
} bl616_bridge_mapping_command_t;

void bl616_bridge_mapping_defaults(
    bl616_bridge_mapping_config_t *config);
void bl616_bridge_mapping_profiles_defaults(
    bl616_bridge_mapping_profiles_t *profiles);
bool bl616_bridge_mapping_validate(
    const bl616_bridge_mapping_config_t *config);
bool bl616_bridge_mapping_profiles_validate(
    const bl616_bridge_mapping_profiles_t *profiles);
bool bl616_bridge_mapping_is_identity(
    const bl616_bridge_mapping_config_t *config);
bool bl616_bridge_mapping_set(
    bl616_bridge_mapping_config_t *config,
    bl616_bridge_button_t target,
    uint8_t source);

/* Copies all non-button state and applies target <- source button bindings. */
bool bl616_bridge_mapping_apply(
    const bl616_bridge_mapping_config_t *config,
    const bl616_bridge_input_state_t *input,
    bl616_bridge_input_state_t *output);

/*
 * Patches only native button/trigger bytes. Touch, motion, timestamps and
 * vendor-specific bytes remain byte-for-byte identical to the source report.
 */
bool bl616_bridge_mapping_patch_native_report(
    const bl616_bridge_mapping_config_t *config,
    bl616_bridge_role_t role,
    const bl616_bridge_input_state_t *input,
    uint8_t *report,
    size_t report_len);

const char *bl616_bridge_button_name(uint8_t button);
bool bl616_bridge_button_parse(const char *name, uint8_t *button);

size_t bl616_bridge_mapping_serialize(
    const bl616_bridge_mapping_config_t *config,
    uint8_t *wire,
    size_t wire_capacity);
bool bl616_bridge_mapping_deserialize(
    const uint8_t *wire,
    size_t wire_len,
    bl616_bridge_mapping_config_t *config);

size_t bl616_bridge_mapping_profiles_serialize(
    const bl616_bridge_mapping_profiles_t *profiles,
    uint8_t *wire,
    size_t wire_capacity);
bool bl616_bridge_mapping_profiles_deserialize(
    const uint8_t *wire,
    size_t wire_len,
    bl616_bridge_mapping_profiles_t *profiles);

const char *bl616_bridge_mapping_profile_name(
    bl616_bridge_mapping_profile_t profile);
bool bl616_bridge_mapping_profile_parse(
    const char *name,
    bl616_bridge_mapping_profile_t *profile);

/* Returns 1 for a valid mapping command, 0 for another command, and -1 for a
 * malformed command beginning with "mapping". */
int bl616_bridge_mapping_parse_command(
    const char *text,
    bl616_bridge_mapping_command_t *command);

/* Formats the manager response used by `mapping get` and successful edits. */
int bl616_bridge_mapping_format_json(
    const bl616_bridge_mapping_config_t *config,
    bool dirty,
    char *json,
    size_t json_len);
int bl616_bridge_mapping_format_profile_json(
    bl616_bridge_mapping_profile_t profile,
    const bl616_bridge_mapping_config_t *config,
    bool dirty,
    char *json,
    size_t json_len);

#ifdef __cplusplus
}
#endif

#endif /* BL616_BRIDGE_MAPPING_H */
