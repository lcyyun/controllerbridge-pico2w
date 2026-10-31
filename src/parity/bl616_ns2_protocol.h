#ifndef BL616_NS2_PROTOCOL_H
#define BL616_NS2_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BL616_NS2_INPUT_REPORT_ID 0x05U
#define BL616_NS2_OUTPUT_REPORT_ID 0x02U
#define BL616_NS2_FEATURE_REPORT_ID 0x7fU
#define BL616_NS2_REPORT_SIZE 64U
#define BL616_NS2_FEATURE_PAYLOAD_OFFSET 11U
#define BL616_NS2_FEATURE_REPLY_CAPACITY 2048U
#define BL616_NS2_FEATURE_MAGIC_SIZE 6U
#define BL616_NS2_NOTIFY_KIND_UNKNOWN 0U
#define BL616_NS2_NOTIFY_KIND_FD2 1U
#define BL616_NS2_NOTIFY_KIND_LEGACY 2U

extern const char BL616_NS2_FEATURE_COMMAND_MAGIC[BL616_NS2_FEATURE_MAGIC_SIZE + 1U];
extern const char BL616_NS2_FEATURE_REPLY_MAGIC[BL616_NS2_FEATURE_MAGIC_SIZE + 1U];

typedef struct {
    const char *profile;
    uint16_t report_rate_hz;
    uint8_t raw_passthrough;
    uint8_t rumble_enabled;
} bl616_ns2_settings_t;

extern const bl616_ns2_settings_t BL616_NS2_DEFAULT_SETTINGS;

typedef enum {
    BL616_NS2_BUTTON_B = 0,
    BL616_NS2_BUTTON_A,
    BL616_NS2_BUTTON_Y,
    BL616_NS2_BUTTON_X,
    BL616_NS2_BUTTON_R,
    BL616_NS2_BUTTON_ZR,
    BL616_NS2_BUTTON_PLUS,
    BL616_NS2_BUTTON_R_STICK,
    BL616_NS2_BUTTON_D_DOWN,
    BL616_NS2_BUTTON_D_RIGHT,
    BL616_NS2_BUTTON_D_LEFT,
    BL616_NS2_BUTTON_D_UP,
    BL616_NS2_BUTTON_L,
    BL616_NS2_BUTTON_ZL,
    BL616_NS2_BUTTON_MINUS,
    BL616_NS2_BUTTON_L_STICK,
    BL616_NS2_BUTTON_HOME,
    BL616_NS2_BUTTON_CAPTURE,
    BL616_NS2_BUTTON_GR,
    BL616_NS2_BUTTON_GL,
    BL616_NS2_BUTTON_C,
    BL616_NS2_BUTTON_COUNT
} bl616_ns2_button_t;

typedef struct {
    uint8_t valid;
    uint8_t kind;
    uint16_t len;
    uint32_t updates;
    uint32_t parse_errors;
    uint32_t buttons;
    uint16_t lx;
    uint16_t ly;
    uint16_t rx;
    uint16_t ry;
    uint8_t motion_valid;
    uint8_t motion[12];
    uint8_t raw_len;
    uint8_t raw[64];
} bl616_ns2_ble_snapshot_t;

typedef struct {
    uint8_t report_id;
    const uint8_t *payload;
    size_t payload_len;
} bl616_ns2_rumble_output_t;

typedef int (*bl616_ns2_snapshot_provider_t)(bl616_ns2_ble_snapshot_t *out,
                                                void *context);
typedef int (*bl616_ns2_rumble_forwarder_t)(const bl616_ns2_rumble_output_t *output,
                                               void *context);

typedef struct {
    bl616_ns2_snapshot_provider_t read_snapshot;
    bl616_ns2_rumble_forwarder_t forward_rumble;
    void *context;
} bl616_ns2_protocol_hooks_t;

typedef struct {
    uint8_t bytes[BL616_NS2_FEATURE_REPLY_CAPACITY];
    uint16_t length;
    uint16_t offset;
} bl616_ns2_feature_reply_t;

typedef struct {
    uint8_t valid;
    const uint8_t *bytes;
    uint16_t length;
} bl616_ns2_feature_command_t;

void bl616_ns2_reset_feature_reply(bl616_ns2_feature_reply_t *reply);
int bl616_ns2_queue_feature_reply(bl616_ns2_feature_reply_t *reply, const char *json);
size_t bl616_ns2_build_feature_reply_chunk(bl616_ns2_feature_reply_t *reply,
                                              uint8_t *report,
                                              size_t report_len);
bl616_ns2_feature_command_t bl616_ns2_parse_feature_command(const uint8_t *report,
                                                                  size_t report_len);
int bl616_ns2_parse_ble_notify(const uint8_t *data,
                                  size_t len,
                                  bl616_ns2_ble_snapshot_t *snapshot);
int bl616_ns2_parse_ble_notify_kind(uint8_t kind,
                                       const uint8_t *data,
                                       size_t len,
                                       bl616_ns2_ble_snapshot_t *snapshot);
void bl616_ns2_reset_axis_calibration(void);

void bl616_ns2_make_neutral_report(uint8_t report[BL616_NS2_REPORT_SIZE],
                                      uint8_t sequence,
                                      uint32_t timestamp);
void bl616_ns2_make_report_from_snapshot(const bl616_ns2_ble_snapshot_t *snapshot,
                                            uint8_t report[BL616_NS2_REPORT_SIZE],
                                            uint8_t sequence,
                                            uint32_t timestamp);
int bl616_ns2_make_usb_report(const bl616_ns2_ble_snapshot_t *snapshot,
                                 uint8_t raw_passthrough,
                                 uint8_t report[BL616_NS2_REPORT_SIZE],
                                 uint8_t sequence,
                                 uint32_t timestamp);
void bl616_ns2_write_report_timestamp(
    uint8_t report[BL616_NS2_REPORT_SIZE],
    uint32_t timestamp);
int bl616_ns2_make_report_from_hooks(const bl616_ns2_protocol_hooks_t *hooks,
                                        uint8_t report[BL616_NS2_REPORT_SIZE],
                                        uint8_t sequence,
                                        uint32_t timestamp);
int bl616_ns2_forward_output_report(const bl616_ns2_protocol_hooks_t *hooks,
                                       const uint8_t *report,
                                       size_t report_len);

#ifdef __cplusplus
}
#endif

#endif /* BL616_NS2_PROTOCOL_H */
