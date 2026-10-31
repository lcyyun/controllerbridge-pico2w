#include "bl616_bridge_protocol.h"

#include <string.h>

_Static_assert(BL616_BRIDGE_BUTTON_COUNT <= 32,
               "unified buttons must fit the uint32_t mask");

enum {
    DS5_REPORT_ID = 0x01,
    DS5_BODY_SIZE = 63,
    DS5_SENSOR_TIMESTAMP_OFFSET = 27,
    DS5_BATTERY_OFFSET = 52,
    NS2PRO_REPORT_ID = 0x05,
    NS2PRO_TIMESTAMP_OFFSET = 0x2b,
    NS2PRO_MOTION_OFFSET = 0x31,
    NS2PRO_STICK_CENTER = 2048,
    NS2PRO_STICK_MAX = 4095,
};

typedef struct {
    uint16_t low_frequency;
    uint16_t high_frequency;
    uint16_t low_amplitude;
    uint16_t high_amplitude;
} nintendo_haptic_t;

static int state_button(const bl616_bridge_input_state_t *state,
                        bl616_bridge_button_t button)
{
    return state != 0 &&
           button < BL616_BRIDGE_BUTTON_COUNT &&
           (state->buttons & BL616_BRIDGE_BUTTON_MASK(button)) != 0U;
}

static int ns2_button(const bl616_ns2_ble_snapshot_t *snapshot,
                      bl616_ns2_button_t button)
{
    return snapshot != 0 &&
           button < BL616_NS2_BUTTON_COUNT &&
           (snapshot->buttons & (UINT32_C(1) << (uint8_t)button)) != 0U;
}

static void set_state_button(bl616_bridge_input_state_t *state,
                             bl616_bridge_button_t button,
                             int pressed)
{
    if (state != 0 && pressed != 0 &&
        button < BL616_BRIDGE_BUTTON_COUNT) {
        state->buttons |= BL616_BRIDGE_BUTTON_MASK(button);
    }
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

static int16_t read_le16s(const uint8_t *data)
{
    uint16_t value = read_le16(data);

    if (value <= UINT16_C(0x7fff)) {
        return (int16_t)value;
    }
    return (int16_t)((int32_t)value - INT32_C(65536));
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static void write_le16(uint8_t *data, int16_t value)
{
    uint16_t wire = (uint16_t)value;

    data[0] = (uint8_t)(wire & UINT16_C(0x00ff));
    data[1] = (uint8_t)((wire >> 8U) & UINT16_C(0x00ff));
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & UINT32_C(0x000000ff));
    data[1] = (uint8_t)((value >> 8U) & UINT32_C(0x000000ff));
    data[2] = (uint8_t)((value >> 16U) & UINT32_C(0x000000ff));
    data[3] = (uint8_t)((value >> 24U) & UINT32_C(0x000000ff));
}

/* Piecewise conversion keeps both endpoints and the exact logical centre. */
static int16_t axis_from_u8(uint8_t value)
{
    if (value >= 128U) {
        uint32_t delta = (uint32_t)value - UINT32_C(128);
        return (int16_t)((delta * UINT32_C(32767) + UINT32_C(63)) /
                         UINT32_C(127));
    }

    return (int16_t)-(int32_t)((((uint32_t)128U - (uint32_t)value) *
                                UINT32_C(32768) + UINT32_C(64)) /
                               UINT32_C(128));
}

static uint8_t axis_to_u8(int16_t value)
{
    int32_t wide = (int32_t)value;

    if (wide >= 0) {
        uint32_t scaled = ((uint32_t)wide * UINT32_C(127) +
                           UINT32_C(16383)) / UINT32_C(32767);
        return (uint8_t)(UINT32_C(128) + scaled);
    }

    {
        uint32_t magnitude = (uint32_t)(-wide);
        uint32_t scaled = (magnitude * UINT32_C(128) + UINT32_C(16384)) /
                          UINT32_C(32768);
        return (uint8_t)(UINT32_C(128) - scaled);
    }
}

static int16_t axis_from_u12(uint16_t value)
{
    uint32_t clamped = value > NS2PRO_STICK_MAX ?
                       (uint32_t)NS2PRO_STICK_MAX : (uint32_t)value;

    if (clamped >= NS2PRO_STICK_CENTER) {
        uint32_t delta = clamped - (uint32_t)NS2PRO_STICK_CENTER;
        return (int16_t)((delta * UINT32_C(32767) + UINT32_C(1023)) /
                         UINT32_C(2047));
    }

    return (int16_t)-(int32_t)((((uint32_t)NS2PRO_STICK_CENTER - clamped) *
                                UINT32_C(32768) + UINT32_C(1024)) /
                               UINT32_C(2048));
}

static uint16_t axis_to_u12(int16_t value)
{
    int32_t wide = (int32_t)value;

    if (wide >= 0) {
        uint32_t scaled = ((uint32_t)wide * UINT32_C(2047) +
                           UINT32_C(16383)) / UINT32_C(32767);
        return (uint16_t)(UINT32_C(2048) + scaled);
    }

    {
        uint32_t magnitude = (uint32_t)(-wide);
        uint32_t scaled = (magnitude * UINT32_C(2048) + UINT32_C(16384)) /
                          UINT32_C(32768);
        return (uint16_t)(UINT32_C(2048) - scaled);
    }
}

static uint8_t trigger_to_u8(uint16_t value)
{
    return (uint8_t)(((uint32_t)value + UINT32_C(128)) / UINT32_C(257));
}

int bl616_bridge_encode_ds5_output(
    const bl616_bridge_feedback_t *feedback,
    uint8_t report[BL616_BRIDGE_DS5_OUTPUT_REPORT_SIZE])
{
    uint8_t *body;

    if (feedback == 0 || report == 0) {
        return -1;
    }

    memset(report, 0, BL616_BRIDGE_DS5_OUTPUT_REPORT_SIZE);
    report[0] = 0x02U;
    body = report + 1U;
    if (feedback->valid == 0U) {
        return 0;
    }

    /*
     * Enable rumble emulation.  Bit 1 explicitly requests classic motors;
     * leaving it clear preserves a DualSense-haptics request marker.
     */
    body[0] = 0x01U;
    if (feedback->type != BL616_BRIDGE_FEEDBACK_DUALSENSE_HAPTICS) {
        body[0] |= 0x02U;
    }
    body[2] = trigger_to_u8(feedback->right_motor);
    body[3] = trigger_to_u8(feedback->left_motor);
    return 0;
}

static uint16_t trigger_from_u8(uint8_t value)
{
    return (uint16_t)((uint16_t)value * UINT16_C(257));
}

static int16_t invert_axis(int16_t value)
{
    if (value == INT16_MIN) {
        return INT16_MAX;
    }
    if (value == INT16_MAX) {
        return INT16_MIN;
    }
    return (int16_t)-value;
}

static void pack12_pair(uint8_t *report, size_t offset, uint16_t x, uint16_t y)
{
    report[offset] = (uint8_t)(x & UINT16_C(0x00ff));
    report[offset + 1U] =
        (uint8_t)(((x >> 8U) & UINT16_C(0x000f)) |
                  ((y & UINT16_C(0x000f)) << 4U));
    report[offset + 2U] = (uint8_t)((y >> 4U) & UINT16_C(0x00ff));
}

static uint8_t ds5_dpad(const bl616_bridge_input_state_t *state)
{
    int up = state_button(state, BL616_BRIDGE_BUTTON_DPAD_UP);
    int down = state_button(state, BL616_BRIDGE_BUTTON_DPAD_DOWN);
    int left = state_button(state, BL616_BRIDGE_BUTTON_DPAD_LEFT);
    int right = state_button(state, BL616_BRIDGE_BUTTON_DPAD_RIGHT);

    if (up != 0 && down != 0) {
        up = 0;
        down = 0;
    }
    if (left != 0 && right != 0) {
        left = 0;
        right = 0;
    }

    if (up != 0) {
        if (right != 0) return 1U;
        if (left != 0) return 7U;
        return 0U;
    }
    if (down != 0) {
        if (right != 0) return 3U;
        if (left != 0) return 5U;
        return 4U;
    }
    if (right != 0) return 2U;
    if (left != 0) return 6U;
    return 8U;
}

static void decode_ds5_dpad(uint8_t dpad,
                            bl616_bridge_input_state_t *state)
{
    set_state_button(state, BL616_BRIDGE_BUTTON_DPAD_UP,
                     dpad == 0U || dpad == 1U || dpad == 7U);
    set_state_button(state, BL616_BRIDGE_BUTTON_DPAD_RIGHT,
                     dpad == 1U || dpad == 2U || dpad == 3U);
    set_state_button(state, BL616_BRIDGE_BUTTON_DPAD_DOWN,
                     dpad == 3U || dpad == 4U || dpad == 5U);
    set_state_button(state, BL616_BRIDGE_BUTTON_DPAD_LEFT,
                     dpad == 5U || dpad == 6U || dpad == 7U);
}

void bl616_bridge_input_state_reset(bl616_bridge_input_state_t *state)
{
    if (state == 0) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->battery_percent = BL616_BRIDGE_BATTERY_UNKNOWN;
}

void bl616_bridge_feedback_reset(bl616_bridge_feedback_t *feedback)
{
    if (feedback != 0) {
        memset(feedback, 0, sizeof(*feedback));
    }
}

bl616_bridge_output_route_t bl616_bridge_select_output_route(
    bl616_bridge_role_t role,
    bl616_bridge_input_source_t source,
    const uint8_t *report,
    size_t report_len)
{
    static const uint8_t manager_magic[BL616_NS2_FEATURE_MAGIC_SIZE] = {
        'Y', '7', 'H', 'I', 'D', '1'
    };

    if (report == 0 || report_len == 0U ||
        role == BL616_BRIDGE_ROLE_UNKNOWN) {
        return BL616_BRIDGE_OUTPUT_ROUTE_DROP;
    }
    if (role == BL616_BRIDGE_ROLE_NS2PRO &&
        report[0] == BL616_NS2_OUTPUT_REPORT_ID &&
        report_len > (size_t)(BL616_NS2_FEATURE_MAGIC_SIZE + 1U) &&
        memcmp(report + 1U, manager_magic,
               BL616_NS2_FEATURE_MAGIC_SIZE) == 0) {
        return BL616_BRIDGE_OUTPUT_ROUTE_MANAGER;
    }
    if (source == BL616_BRIDGE_INPUT_SOURCE_NONE) {
        return BL616_BRIDGE_OUTPUT_ROUTE_DROP;
    }
    if ((role == BL616_BRIDGE_ROLE_DUALSENSE ||
         role == BL616_BRIDGE_ROLE_DUALSENSE_EDGE) &&
        source == BL616_BRIDGE_INPUT_SOURCE_DUALSENSE_BT) {
        return BL616_BRIDGE_OUTPUT_ROUTE_DS5_NATIVE;
    }
    if (role == BL616_BRIDGE_ROLE_NS2PRO &&
        source == BL616_BRIDGE_INPUT_SOURCE_NS2PRO_BLE) {
        return BL616_BRIDGE_OUTPUT_ROUTE_NS2_NATIVE;
    }
    return BL616_BRIDGE_OUTPUT_ROUTE_TRANSLATED;
}

int bl616_bridge_state_from_ns2_snapshot(
    const bl616_ns2_ble_snapshot_t *snapshot,
    uint32_t sensor_timestamp,
    bl616_bridge_input_state_t *out)
{
    if (out == 0) {
        return -1;
    }

    bl616_bridge_input_state_reset(out);
    out->source = BL616_BRIDGE_INPUT_SOURCE_NS2PRO_BLE;
    if (snapshot == 0 || snapshot->valid == 0U) {
        return -1;
    }

    set_state_button(out, BL616_BRIDGE_BUTTON_SOUTH,
                     ns2_button(snapshot, BL616_NS2_BUTTON_B));
    set_state_button(out, BL616_BRIDGE_BUTTON_EAST,
                     ns2_button(snapshot, BL616_NS2_BUTTON_A));
    set_state_button(out, BL616_BRIDGE_BUTTON_WEST,
                     ns2_button(snapshot, BL616_NS2_BUTTON_Y));
    set_state_button(out, BL616_BRIDGE_BUTTON_NORTH,
                     ns2_button(snapshot, BL616_NS2_BUTTON_X));
    set_state_button(out, BL616_BRIDGE_BUTTON_DPAD_UP,
                     ns2_button(snapshot, BL616_NS2_BUTTON_D_UP));
    set_state_button(out, BL616_BRIDGE_BUTTON_DPAD_DOWN,
                     ns2_button(snapshot, BL616_NS2_BUTTON_D_DOWN));
    set_state_button(out, BL616_BRIDGE_BUTTON_DPAD_LEFT,
                     ns2_button(snapshot, BL616_NS2_BUTTON_D_LEFT));
    set_state_button(out, BL616_BRIDGE_BUTTON_DPAD_RIGHT,
                     ns2_button(snapshot, BL616_NS2_BUTTON_D_RIGHT));
    set_state_button(out, BL616_BRIDGE_BUTTON_LEFT_SHOULDER,
                     ns2_button(snapshot, BL616_NS2_BUTTON_L));
    set_state_button(out, BL616_BRIDGE_BUTTON_RIGHT_SHOULDER,
                     ns2_button(snapshot, BL616_NS2_BUTTON_R));
    set_state_button(out, BL616_BRIDGE_BUTTON_LEFT_TRIGGER,
                     ns2_button(snapshot, BL616_NS2_BUTTON_ZL));
    set_state_button(out, BL616_BRIDGE_BUTTON_RIGHT_TRIGGER,
                     ns2_button(snapshot, BL616_NS2_BUTTON_ZR));
    set_state_button(out, BL616_BRIDGE_BUTTON_BACK,
                     ns2_button(snapshot, BL616_NS2_BUTTON_MINUS));
    set_state_button(out, BL616_BRIDGE_BUTTON_START,
                     ns2_button(snapshot, BL616_NS2_BUTTON_PLUS));
    set_state_button(out, BL616_BRIDGE_BUTTON_LEFT_STICK,
                     ns2_button(snapshot, BL616_NS2_BUTTON_L_STICK));
    set_state_button(out, BL616_BRIDGE_BUTTON_RIGHT_STICK,
                     ns2_button(snapshot, BL616_NS2_BUTTON_R_STICK));
    set_state_button(out, BL616_BRIDGE_BUTTON_GUIDE,
                     ns2_button(snapshot, BL616_NS2_BUTTON_HOME));
    set_state_button(out, BL616_BRIDGE_BUTTON_CAPTURE,
                     ns2_button(snapshot, BL616_NS2_BUTTON_CAPTURE));
    set_state_button(out, BL616_BRIDGE_BUTTON_LEFT_PADDLE,
                     ns2_button(snapshot, BL616_NS2_BUTTON_GL));
    set_state_button(out, BL616_BRIDGE_BUTTON_RIGHT_PADDLE,
                     ns2_button(snapshot, BL616_NS2_BUTTON_GR));
    set_state_button(out, BL616_BRIDGE_BUTTON_C,
                     ns2_button(snapshot, BL616_NS2_BUTTON_C));

    out->left_x = axis_from_u12(snapshot->lx);
    out->left_y = axis_from_u12(snapshot->ly);
    out->right_x = axis_from_u12(snapshot->rx);
    out->right_y = axis_from_u12(snapshot->ry);
    out->left_trigger = ns2_button(snapshot, BL616_NS2_BUTTON_ZL) ?
                        UINT16_MAX : 0U;
    out->right_trigger = ns2_button(snapshot, BL616_NS2_BUTTON_ZR) ?
                         UINT16_MAX : 0U;

    if (snapshot->motion_valid != 0U) {
        size_t i;
        for (i = 0U; i < 3U; ++i) {
            out->accel[i] = read_le16s(snapshot->motion + i * 2U);
            out->gyro[i] = read_le16s(snapshot->motion + 6U + i * 2U);
        }
        out->motion_valid = 1U;
    }

    out->sensor_timestamp = sensor_timestamp;
    out->timestamp_valid = 1U;
    out->valid = 1U;
    return 0;
}

int bl616_bridge_parse_ds5_usb_input(const uint8_t *report,
                                        size_t report_len,
                                        bl616_bridge_input_state_t *out)
{
    const uint8_t *body;
    uint8_t b7;
    uint8_t b8;
    uint8_t b9;
    uint8_t battery_level;

    if (out == 0) {
        return -1;
    }

    bl616_bridge_input_state_reset(out);
    out->source = BL616_BRIDGE_INPUT_SOURCE_DUALSENSE_BT;
    if (report == 0) {
        return -1;
    }

    if (report_len == DS5_BODY_SIZE) {
        body = report;
    } else if (report_len == BL616_BRIDGE_DS5_INPUT_REPORT_SIZE &&
               report[0] == DS5_REPORT_ID) {
        body = report + 1U;
    } else {
        return -1;
    }

    out->left_x = axis_from_u8(body[0]);
    out->left_y = invert_axis(axis_from_u8(body[1]));
    out->right_x = axis_from_u8(body[2]);
    out->right_y = invert_axis(axis_from_u8(body[3]));
    out->left_trigger = trigger_from_u8(body[4]);
    out->right_trigger = trigger_from_u8(body[5]);

    b7 = body[7];
    b8 = body[8];
    b9 = body[9];
    decode_ds5_dpad((uint8_t)(b7 & 0x0fU), out);
    set_state_button(out, BL616_BRIDGE_BUTTON_WEST, (b7 & 0x10U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_SOUTH, (b7 & 0x20U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_EAST, (b7 & 0x40U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_NORTH, (b7 & 0x80U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_LEFT_SHOULDER,
                     (b8 & 0x01U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_RIGHT_SHOULDER,
                     (b8 & 0x02U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_LEFT_TRIGGER,
                     (b8 & 0x04U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_RIGHT_TRIGGER,
                     (b8 & 0x08U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_BACK, (b8 & 0x10U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_START, (b8 & 0x20U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_LEFT_STICK,
                     (b8 & 0x40U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_RIGHT_STICK,
                     (b8 & 0x80U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_GUIDE, (b9 & 0x01U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_TOUCHPAD, (b9 & 0x02U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_MUTE, (b9 & 0x04U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_LEFT_FUNCTION,
                     (b9 & 0x10U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_RIGHT_FUNCTION,
                     (b9 & 0x20U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_LEFT_PADDLE,
                     (b9 & 0x40U) != 0U);
    set_state_button(out, BL616_BRIDGE_BUTTON_RIGHT_PADDLE,
                     (b9 & 0x80U) != 0U);

    out->gyro[0] = read_le16s(body + 15U);
    out->gyro[2] = read_le16s(body + 17U);
    out->gyro[1] = read_le16s(body + 19U);
    out->accel[0] = read_le16s(body + 21U);
    out->accel[1] = read_le16s(body + 23U);
    out->accel[2] = read_le16s(body + 25U);
    out->motion_valid = 1U;
    out->sensor_timestamp = read_le32(body + DS5_SENSOR_TIMESTAMP_OFFSET);
    out->timestamp_valid = 1U;

    battery_level = (uint8_t)(body[DS5_BATTERY_OFFSET] & 0x0fU);
    if (battery_level <= 10U) {
        out->battery_percent = (uint8_t)(battery_level * 10U);
        out->battery_valid = 1U;
    }

    out->valid = 1U;
    return 0;
}

int bl616_bridge_encode_xinput(
    const bl616_bridge_input_state_t *state,
    uint8_t report[BL616_BRIDGE_XINPUT_REPORT_SIZE])
{
    if (state == 0 || report == 0) {
        return -1;
    }

    memset(report, 0, BL616_BRIDGE_XINPUT_REPORT_SIZE);
    report[0] = 0x00U;
    report[1] = (uint8_t)BL616_BRIDGE_XINPUT_REPORT_SIZE;
    if (state->valid == 0U) {
        return 0;
    }

    if (state_button(state, BL616_BRIDGE_BUTTON_DPAD_UP)) report[2] |= 0x01U;
    if (state_button(state, BL616_BRIDGE_BUTTON_DPAD_DOWN)) report[2] |= 0x02U;
    if (state_button(state, BL616_BRIDGE_BUTTON_DPAD_LEFT)) report[2] |= 0x04U;
    if (state_button(state, BL616_BRIDGE_BUTTON_DPAD_RIGHT)) report[2] |= 0x08U;
    if (state_button(state, BL616_BRIDGE_BUTTON_START)) report[2] |= 0x10U;
    if (state_button(state, BL616_BRIDGE_BUTTON_BACK) ||
        state_button(state, BL616_BRIDGE_BUTTON_CAPTURE)) report[2] |= 0x20U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_STICK)) report[2] |= 0x40U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_STICK)) report[2] |= 0x80U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_SHOULDER)) report[3] |= 0x01U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_SHOULDER)) report[3] |= 0x02U;
    if (state_button(state, BL616_BRIDGE_BUTTON_GUIDE)) report[3] |= 0x04U;
    if (state_button(state, BL616_BRIDGE_BUTTON_SOUTH)) report[3] |= 0x10U;
    if (state_button(state, BL616_BRIDGE_BUTTON_EAST)) report[3] |= 0x20U;
    if (state_button(state, BL616_BRIDGE_BUTTON_WEST)) report[3] |= 0x40U;
    if (state_button(state, BL616_BRIDGE_BUTTON_NORTH)) report[3] |= 0x80U;

    report[4] = trigger_to_u8(state->left_trigger);
    report[5] = trigger_to_u8(state->right_trigger);
    write_le16(report + 6U, state->left_x);
    write_le16(report + 8U, state->left_y);
    write_le16(report + 10U, state->right_x);
    write_le16(report + 12U, state->right_y);
    return 0;
}

int bl616_bridge_encode_ds5_input(
    const bl616_bridge_input_state_t *state,
    uint8_t sequence,
    uint8_t report[BL616_BRIDGE_DS5_INPUT_REPORT_SIZE])
{
    uint8_t *body;
    uint8_t battery_level;

    if (state == 0 || report == 0) {
        return -1;
    }

    memset(report, 0, BL616_BRIDGE_DS5_INPUT_REPORT_SIZE);
    report[0] = DS5_REPORT_ID;
    body = report + 1U;
    body[0] = 128U;
    body[1] = 128U;
    body[2] = 128U;
    body[3] = 128U;
    body[6] = sequence;
    body[7] = 8U;
    body[32] = 0x80U; /* Both touch contacts are explicitly not touching. */
    body[36] = 0x80U;
    body[DS5_BATTERY_OFFSET] = 0x2aU; /* Unknown -> complete, avoids false low alert. */
    if (state->valid == 0U) {
        return 0;
    }

    body[0] = axis_to_u8(state->left_x);
    body[1] = axis_to_u8(invert_axis(state->left_y));
    body[2] = axis_to_u8(state->right_x);
    body[3] = axis_to_u8(invert_axis(state->right_y));
    body[4] = trigger_to_u8(state->left_trigger);
    body[5] = trigger_to_u8(state->right_trigger);
    body[7] = ds5_dpad(state);
    if (state_button(state, BL616_BRIDGE_BUTTON_WEST)) body[7] |= 0x10U;
    if (state_button(state, BL616_BRIDGE_BUTTON_SOUTH)) body[7] |= 0x20U;
    if (state_button(state, BL616_BRIDGE_BUTTON_EAST)) body[7] |= 0x40U;
    if (state_button(state, BL616_BRIDGE_BUTTON_NORTH)) body[7] |= 0x80U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_SHOULDER)) body[8] |= 0x01U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_SHOULDER)) body[8] |= 0x02U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_TRIGGER) ||
        state->left_trigger != 0U) body[8] |= 0x04U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_TRIGGER) ||
        state->right_trigger != 0U) body[8] |= 0x08U;
    if (state_button(state, BL616_BRIDGE_BUTTON_BACK) ||
        state_button(state, BL616_BRIDGE_BUTTON_CAPTURE)) body[8] |= 0x10U;
    if (state_button(state, BL616_BRIDGE_BUTTON_START)) body[8] |= 0x20U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_STICK)) body[8] |= 0x40U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_STICK)) body[8] |= 0x80U;
    if (state_button(state, BL616_BRIDGE_BUTTON_GUIDE)) body[9] |= 0x01U;
    if (state_button(state, BL616_BRIDGE_BUTTON_TOUCHPAD)) body[9] |= 0x02U;
    if (state_button(state, BL616_BRIDGE_BUTTON_MUTE)) body[9] |= 0x04U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_FUNCTION)) body[9] |= 0x10U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_FUNCTION)) body[9] |= 0x20U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_PADDLE)) body[9] |= 0x40U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_PADDLE)) body[9] |= 0x80U;

    if (state->motion_valid != 0U) {
        write_le16(body + 15U, state->gyro[0]);
        write_le16(body + 17U, state->gyro[2]);
        write_le16(body + 19U, state->gyro[1]);
        write_le16(body + 21U, state->accel[0]);
        write_le16(body + 23U, state->accel[1]);
        write_le16(body + 25U, state->accel[2]);
    }
    if (state->timestamp_valid != 0U) {
        write_le32(body + DS5_SENSOR_TIMESTAMP_OFFSET, state->sensor_timestamp);
    }
    if (state->battery_valid != 0U) {
        uint8_t percent = state->battery_percent > 100U ? 100U :
                          state->battery_percent;
        battery_level = (uint8_t)(((uint16_t)percent + 5U) / 10U);
        body[DS5_BATTERY_OFFSET] = battery_level;
        if (percent >= 100U) {
            body[DS5_BATTERY_OFFSET] |= 0x20U;
        }
    }
    return 0;
}

int bl616_bridge_encode_ns2pro_input(
    const bl616_bridge_input_state_t *state,
    uint8_t sequence,
    uint8_t report[BL616_BRIDGE_NS2PRO_INPUT_REPORT_SIZE])
{
    size_t i;

    if (state == 0 || report == 0) {
        return -1;
    }

    memset(report, 0, BL616_BRIDGE_NS2PRO_INPUT_REPORT_SIZE);
    report[0] = NS2PRO_REPORT_ID;
    report[1] = sequence;
    report[2] = 0x20U;
    pack12_pair(report, 11U, NS2PRO_STICK_CENTER, NS2PRO_STICK_CENTER);
    pack12_pair(report, 14U, NS2PRO_STICK_CENTER, NS2PRO_STICK_CENTER);
    if (state->valid == 0U) {
        return 0;
    }

    if (state_button(state, BL616_BRIDGE_BUTTON_WEST)) report[5] |= 0x01U;
    if (state_button(state, BL616_BRIDGE_BUTTON_NORTH)) report[5] |= 0x02U;
    if (state_button(state, BL616_BRIDGE_BUTTON_SOUTH)) report[5] |= 0x04U;
    if (state_button(state, BL616_BRIDGE_BUTTON_EAST)) report[5] |= 0x08U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_SHOULDER)) report[5] |= 0x40U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_TRIGGER) ||
        state->right_trigger != 0U) report[5] |= 0x80U;
    if (state_button(state, BL616_BRIDGE_BUTTON_BACK)) report[6] |= 0x01U;
    if (state_button(state, BL616_BRIDGE_BUTTON_START)) report[6] |= 0x02U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_STICK)) report[6] |= 0x04U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_STICK)) report[6] |= 0x08U;
    if (state_button(state, BL616_BRIDGE_BUTTON_GUIDE)) report[6] |= 0x10U;
    if (state_button(state, BL616_BRIDGE_BUTTON_CAPTURE) ||
        state_button(state, BL616_BRIDGE_BUTTON_TOUCHPAD)) report[6] |= 0x20U;
    if (state_button(state, BL616_BRIDGE_BUTTON_C)) report[6] |= 0x40U;
    if (state_button(state, BL616_BRIDGE_BUTTON_DPAD_DOWN)) report[7] |= 0x01U;
    if (state_button(state, BL616_BRIDGE_BUTTON_DPAD_UP)) report[7] |= 0x02U;
    if (state_button(state, BL616_BRIDGE_BUTTON_DPAD_RIGHT)) report[7] |= 0x04U;
    if (state_button(state, BL616_BRIDGE_BUTTON_DPAD_LEFT)) report[7] |= 0x08U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_SHOULDER)) report[7] |= 0x40U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_TRIGGER) ||
        state->left_trigger != 0U) report[7] |= 0x80U;
    if (state_button(state, BL616_BRIDGE_BUTTON_RIGHT_PADDLE)) report[8] |= 0x01U;
    if (state_button(state, BL616_BRIDGE_BUTTON_LEFT_PADDLE)) report[8] |= 0x02U;

    pack12_pair(report, 11U, axis_to_u12(state->left_x), axis_to_u12(state->left_y));
    pack12_pair(report, 14U, axis_to_u12(state->right_x), axis_to_u12(state->right_y));
    if (state->timestamp_valid != 0U) {
        write_le32(report + NS2PRO_TIMESTAMP_OFFSET, state->sensor_timestamp);
    }
    if (state->motion_valid != 0U) {
        for (i = 0U; i < 3U; ++i) {
            write_le16(report + NS2PRO_MOTION_OFFSET + i * 2U,
                       state->accel[i]);
            write_le16(report + NS2PRO_MOTION_OFFSET + 6U + i * 2U,
                       state->gyro[i]);
        }
    }
    return 0;
}

size_t bl616_bridge_input_report_size(bl616_bridge_role_t role)
{
    switch (role) {
        case BL616_BRIDGE_ROLE_XBOX_360:
            return BL616_BRIDGE_XINPUT_REPORT_SIZE;
        case BL616_BRIDGE_ROLE_DUALSENSE:
        case BL616_BRIDGE_ROLE_DUALSENSE_EDGE:
            return BL616_BRIDGE_DS5_INPUT_REPORT_SIZE;
        case BL616_BRIDGE_ROLE_NS2PRO:
            return BL616_BRIDGE_NS2PRO_INPUT_REPORT_SIZE;
        case BL616_BRIDGE_ROLE_UNKNOWN:
        default:
            return 0U;
    }
}

int bl616_bridge_encode_input(bl616_bridge_role_t role,
                                 const bl616_bridge_input_state_t *state,
                                 uint8_t sequence,
                                 uint8_t *report,
                                 size_t report_capacity)
{
    size_t required = bl616_bridge_input_report_size(role);

    if (state == 0 || report == 0 || required == 0U ||
        report_capacity < required) {
        return -1;
    }

    switch (role) {
        case BL616_BRIDGE_ROLE_XBOX_360:
            return bl616_bridge_encode_xinput(state, report);
        case BL616_BRIDGE_ROLE_DUALSENSE:
        case BL616_BRIDGE_ROLE_DUALSENSE_EDGE:
            return bl616_bridge_encode_ds5_input(state, sequence, report);
        case BL616_BRIDGE_ROLE_NS2PRO:
            return bl616_bridge_encode_ns2pro_input(state, sequence, report);
        case BL616_BRIDGE_ROLE_UNKNOWN:
        default:
            return -1;
    }
}

int bl616_bridge_decode_xbox_output(const uint8_t *report,
                                       size_t report_len,
                                       bl616_bridge_feedback_t *out)
{
    uint8_t left;
    uint8_t right;

    if (out == 0) {
        return -1;
    }
    bl616_bridge_feedback_reset(out);
    if (report == 0) {
        return -1;
    }

    if (report_len >= 5U && report[0] == 0x00U && report[1] == 0x08U) {
        left = report[3];
        right = report[4];
    } else if (report_len >= 4U && report[0] == 0x08U) {
        left = report[2];
        right = report[3];
    } else {
        return -1;
    }

    out->left_motor = trigger_from_u8(left);
    out->right_motor = trigger_from_u8(right);
    out->type = BL616_BRIDGE_FEEDBACK_DUAL_MOTOR;
    out->valid = 1U;
    return 0;
}

int bl616_bridge_decode_ds5_output(const uint8_t *report,
                                      size_t report_len,
                                      bl616_bridge_feedback_t *out)
{
    const uint8_t *body;
    size_t body_len;

    if (out == 0) {
        return -1;
    }
    bl616_bridge_feedback_reset(out);
    if (report == 0) {
        return -1;
    }

    if (report_len >= 5U && report[0] == 0x02U) {
        body = report + 1U;
        body_len = report_len - 1U;
    } else {
        body = report;
        body_len = report_len;
    }
    if (body_len < 4U) {
        return -1;
    }

    /* USB body offsets 2/3 are right (light) and left (heavy) rumble. */
    out->right_motor = trigger_from_u8(body[2]);
    out->left_motor = trigger_from_u8(body[3]);
    out->type = (body[0] & 0x02U) != 0U ?
                BL616_BRIDGE_FEEDBACK_DUALSENSE_RUMBLE :
                BL616_BRIDGE_FEEDBACK_DUALSENSE_HAPTICS;
    out->valid = 1U;
    return 0;
}

static void decode_nintendo_haptic(const uint8_t *frame,
                                   nintendo_haptic_t *out)
{
    uint8_t b0 = frame[0];
    uint8_t b1 = frame[1];
    uint8_t b2 = frame[2];
    uint8_t b3 = frame[3];
    uint8_t b4 = frame[4];

    out->high_frequency =
        (uint16_t)((uint16_t)b0 | (((uint16_t)b1 & 0x03U) << 8U));
    out->high_amplitude =
        (uint16_t)((((uint16_t)b1 & 0xfcU) << 4U) |
                   (((uint16_t)b2 & 0x0fU) << 12U));
    out->low_frequency =
        (uint16_t)((((uint16_t)b2 & 0xf0U) >> 4U) |
                   (((uint16_t)b3 & 0x3fU) << 4U));
    out->low_amplitude =
        (uint16_t)(((uint16_t)b3 & 0xc0U) | ((uint16_t)b4 << 8U));
}

static uint16_t max_u16(uint16_t a, uint16_t b)
{
    return a > b ? a : b;
}

int bl616_bridge_decode_ns2pro_output(const uint8_t *report,
                                         size_t report_len,
                                         bl616_bridge_feedback_t *out)
{
    size_t left_offset;
    size_t right_offset;
    nintendo_haptic_t left;
    nintendo_haptic_t right;

    if (out == 0) {
        return -1;
    }
    bl616_bridge_feedback_reset(out);
    if (report == 0) {
        return -1;
    }

    if (report_len >= 23U && report[0] == 0x02U &&
        (report[1] & 0xf0U) == 0x50U) {
        left_offset = 2U;
        right_offset = 0x12U;
    } else if (report_len >= 22U && (report[0] & 0xf0U) == 0x50U) {
        left_offset = 1U;
        right_offset = 0x11U;
    } else {
        return -1;
    }

    decode_nintendo_haptic(report + left_offset, &left);
    decode_nintendo_haptic(report + right_offset, &right);
    out->left_low_amplitude = left.low_amplitude;
    out->left_high_amplitude = left.high_amplitude;
    out->right_low_amplitude = right.low_amplitude;
    out->right_high_amplitude = right.high_amplitude;
    out->left_low_frequency = left.low_frequency;
    out->left_high_frequency = left.high_frequency;
    out->right_low_frequency = right.low_frequency;
    out->right_high_frequency = right.high_frequency;
    out->left_motor = max_u16(left.low_amplitude, left.high_amplitude);
    out->right_motor = max_u16(right.low_amplitude, right.high_amplitude);
    out->type = BL616_BRIDGE_FEEDBACK_NINTENDO_HD;
    out->valid = 1U;
    return 0;
}
