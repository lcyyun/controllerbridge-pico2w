#ifndef CONTROLLERBRIDGE_BRIDGE_FEATURES_H
#define CONTROLLERBRIDGE_BRIDGE_FEATURES_H
#include <cstddef>
#include <cstdint>

// Local compatibility features for a Sony USB role with no native Sony input.
// Returns the TinyUSB feature body (no report ID), truncated to host capacity.
// Does not issue radio requests, wait, or manufacture physical-controller data.
size_t bridge_features_get_fallback(uint8_t report_id, uint8_t *body, size_t capacity);
#endif
