#ifndef BL616_BRIDGE_PROTOCOL_H
#define BL616_BRIDGE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "bl616_ns2_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL616_BRIDGE_XINPUT_REPORT_SIZE 20U
#define BL616_BRIDGE_DS5_INPUT_REPORT_SIZE 64U
#define BL616_BRIDGE_DS5_OUTPUT_REPORT_SIZE 48U
#define BL616_BRIDGE_NS2PRO_INPUT_REPORT_SIZE 64U
#define BL616_BRIDGE_BATTERY_UNKNOWN 0xffU

/*
 * USB personalities exposed by the bridge.  These values are protocol-layer
 * identifiers only; they are intentionally independent of USB implementation
 * and persistent-storage layout.
 */
typedef enum {
    BL616_BRIDGE_ROLE_UNKNOWN = 0,
    BL616_BRIDGE_ROLE_XBOX_360,
    BL616_BRIDGE_ROLE_DUALSENSE,
    BL616_BRIDGE_ROLE_NS2PRO,
    BL616_BRIDGE_ROLE_DUALSENSE_EDGE
} bl616_bridge_role_t;

typedef enum {
    BL616_BRIDGE_INPUT_SOURCE_NONE = 0,
    BL616_BRIDGE_INPUT_SOURCE_DUALSENSE_BT,
    BL616_BRIDGE_INPUT_SOURCE_NS2PRO_BLE
} bl616_bridge_input_source_t;

/*
 * Face buttons use physical positions, not printed labels:
 *
 *   position  Xbox  DualSense  Nintendo
 *   south     A     Cross      B
 *   east      B     Circle     A
 *   west      X     Square     Y
 *   north     Y     Triangle   X
 *
 * This keeps conversion stable regardless of the selected USB personality.
 * Canonical Back maps to Xbox Back / DS5 Create / Nintendo Minus. Runtime
 * adapts an unmodified NS2 Minus binding to DS5/Edge touchpad click while
 * keeping Capture -> Create; explicit Back/Touchpad remaps take precedence.
 * Start maps to Xbox
 * Start / DS5 Options / Nintendo Plus; Guide maps to Xbox Guide / PS / Home.
 * Capture falls back to Back/Create where a role has no dedicated capture
 * button.  DS5 touchpad falls back to Nintendo Capture.  Edge paddles map to
 * NS2Pro GL/GR and are omitted by XInput.
 */
typedef enum {
    BL616_BRIDGE_BUTTON_SOUTH = 0,
    BL616_BRIDGE_BUTTON_EAST,
    BL616_BRIDGE_BUTTON_WEST,
    BL616_BRIDGE_BUTTON_NORTH,
    BL616_BRIDGE_BUTTON_DPAD_UP,
    BL616_BRIDGE_BUTTON_DPAD_DOWN,
    BL616_BRIDGE_BUTTON_DPAD_LEFT,
    BL616_BRIDGE_BUTTON_DPAD_RIGHT,
    BL616_BRIDGE_BUTTON_LEFT_SHOULDER,
    BL616_BRIDGE_BUTTON_RIGHT_SHOULDER,
    BL616_BRIDGE_BUTTON_LEFT_TRIGGER,
    BL616_BRIDGE_BUTTON_RIGHT_TRIGGER,
    BL616_BRIDGE_BUTTON_BACK,
    BL616_BRIDGE_BUTTON_START,
    BL616_BRIDGE_BUTTON_LEFT_STICK,
    BL616_BRIDGE_BUTTON_RIGHT_STICK,
    BL616_BRIDGE_BUTTON_GUIDE,
    BL616_BRIDGE_BUTTON_TOUCHPAD,
    BL616_BRIDGE_BUTTON_MUTE,
    BL616_BRIDGE_BUTTON_CAPTURE,
    BL616_BRIDGE_BUTTON_LEFT_PADDLE,
    BL616_BRIDGE_BUTTON_RIGHT_PADDLE,
    BL616_BRIDGE_BUTTON_LEFT_FUNCTION,
    BL616_BRIDGE_BUTTON_RIGHT_FUNCTION,
    BL616_BRIDGE_BUTTON_C,
    BL616_BRIDGE_BUTTON_COUNT
} bl616_bridge_button_t;

#define BL616_BRIDGE_BUTTON_MASK(button) \
    (UINT32_C(1) << (uint8_t)(button))

/*
 * Stick coordinates cover the full int16_t range. X grows to the right and Y
 * grows up. Trigger values cover 0..65535. Motion samples retain the raw
 * signed 16-bit sensor units while normalising wire ordering to XYZ. NS2Pro
 * and XInput use the canonical direction directly; DS5's unsigned
 * positive-down Y axis is inverted at its input and output boundaries. The
 * 8-bit and 12-bit codecs saturate both endpoints.
 */
typedef struct {
    uint8_t valid;
    bl616_bridge_input_source_t source;
    uint32_t buttons;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
    uint16_t left_trigger;
    uint16_t right_trigger;
    int16_t accel[3];
    int16_t gyro[3];
    uint32_t sensor_timestamp;
    uint8_t battery_percent;
    uint8_t motion_valid;
    uint8_t timestamp_valid;
    uint8_t battery_valid;
} bl616_bridge_input_state_t;

typedef enum {
    BL616_BRIDGE_FEEDBACK_NONE = 0,
    BL616_BRIDGE_FEEDBACK_DUAL_MOTOR,
    BL616_BRIDGE_FEEDBACK_DUALSENSE_RUMBLE,
    BL616_BRIDGE_FEEDBACK_DUALSENSE_HAPTICS,
    BL616_BRIDGE_FEEDBACK_NINTENDO_HD
} bl616_bridge_feedback_type_t;

typedef enum {
    BL616_BRIDGE_OUTPUT_ROUTE_DROP = 0,
    BL616_BRIDGE_OUTPUT_ROUTE_MANAGER,
    BL616_BRIDGE_OUTPUT_ROUTE_DS5_NATIVE,
    BL616_BRIDGE_OUTPUT_ROUTE_NS2_NATIVE,
    BL616_BRIDGE_OUTPUT_ROUTE_TRANSLATED
} bl616_bridge_output_route_t;

/*
 * left_motor/right_motor are normalised 0..65535 amplitudes and are always
 * populated for a valid feedback report.  Nintendo HD reports additionally
 * expose their low/high components so a transport can retain frequency detail.
 */
typedef struct {
    uint8_t valid;
    bl616_bridge_feedback_type_t type;
    uint16_t left_motor;
    uint16_t right_motor;
    uint16_t left_low_amplitude;
    uint16_t left_high_amplitude;
    uint16_t right_low_amplitude;
    uint16_t right_high_amplitude;
    uint16_t left_low_frequency;
    uint16_t left_high_frequency;
    uint16_t right_low_frequency;
    uint16_t right_high_frequency;
} bl616_bridge_feedback_t;

void bl616_bridge_input_state_reset(bl616_bridge_input_state_t *state);
void bl616_bridge_feedback_reset(bl616_bridge_feedback_t *feedback);

bl616_bridge_output_route_t bl616_bridge_select_output_route(
    bl616_bridge_role_t role,
    bl616_bridge_input_source_t source,
    const uint8_t *report,
    size_t report_len);

int bl616_bridge_state_from_ns2_snapshot(
    const bl616_ns2_ble_snapshot_t *snapshot,
    uint32_t sensor_timestamp,
    bl616_bridge_input_state_t *out);

/* Accepts either a 63-byte USB report body or a 64-byte report starting 0x01. */
int bl616_bridge_parse_ds5_usb_input(const uint8_t *report,
                                        size_t report_len,
                                        bl616_bridge_input_state_t *out);

int bl616_bridge_encode_xinput(
    const bl616_bridge_input_state_t *state,
    uint8_t report[BL616_BRIDGE_XINPUT_REPORT_SIZE]);
int bl616_bridge_encode_ds5_input(
    const bl616_bridge_input_state_t *state,
    uint8_t sequence,
    uint8_t report[BL616_BRIDGE_DS5_INPUT_REPORT_SIZE]);
int bl616_bridge_encode_ns2pro_input(
    const bl616_bridge_input_state_t *state,
    uint8_t sequence,
    uint8_t report[BL616_BRIDGE_NS2PRO_INPUT_REPORT_SIZE]);

size_t bl616_bridge_input_report_size(bl616_bridge_role_t role);
int bl616_bridge_encode_input(bl616_bridge_role_t role,
                                 const bl616_bridge_input_state_t *state,
                                 uint8_t sequence,
                                 uint8_t *report,
                                 size_t report_capacity);

int bl616_bridge_decode_xbox_output(const uint8_t *report,
                                       size_t report_len,
                                       bl616_bridge_feedback_t *out);
int bl616_bridge_decode_ds5_output(const uint8_t *report,
                                      size_t report_len,
                                      bl616_bridge_feedback_t *out);
int bl616_bridge_decode_ns2pro_output(const uint8_t *report,
                                         size_t report_len,
                                         bl616_bridge_feedback_t *out);

/*
 * Motor wire order is normalised here: Xbox full reports use [3]=left/[4]=right,
 * DS5 bodies use [3]=left-heavy/[2]=right-light, and Nintendo report 0x02 uses
 * five-byte HD frames at full-report offsets 0x02 (left) and 0x12 (right).
 */

/* Build a USB report 0x02 suitable for the DualSense compatibility path. */
int bl616_bridge_encode_ds5_output(
    const bl616_bridge_feedback_t *feedback,
    uint8_t report[BL616_BRIDGE_DS5_OUTPUT_REPORT_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* BL616_BRIDGE_PROTOCOL_H */
