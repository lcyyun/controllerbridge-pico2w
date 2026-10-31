#pragma once
#include <cstddef>
#include <cstdint>
#include "parity/bl616_bridge_mapping.h"
#include "ns2/ns2_input.h"

// Input preference and emulated USB role have independent persistent lifetimes.
void bridge_runtime_init();
void bridge_runtime_reset_input();
bool bridge_runtime_accepts(uint8_t source);
void bridge_runtime_ds5(const uint8_t *body, size_t len, uint32_t now_ms);
void bridge_runtime_ns2(const Ns2InputSnapshot *input, uint32_t now_ms);
bool bridge_runtime_live(uint32_t now_ms);
void bridge_runtime_state(bl616_bridge_input_state_t *out, uint32_t now_ms, bool mapped);
size_t bridge_runtime_report(bl616_bridge_role_t role, uint8_t *out, size_t capacity, uint32_t now_ms, bool allow_native = true);
void bridge_runtime_status(char *out, size_t size, uint32_t now_ms);
bool bridge_runtime_mapping_command(const char *line, char *out, size_t size);
bool bridge_runtime_save();
void bridge_runtime_defaults();
uint8_t bridge_runtime_role();
void bridge_runtime_set_role(uint8_t role);
uint8_t bridge_runtime_preference();
void bridge_runtime_set_preference(uint8_t preference);
// Platform storage is supplied by bridge_store.cpp (host tests use memory).
bool bridge_store_load(uint8_t *wire, size_t size);
bool bridge_store_save(const uint8_t *wire, size_t size);
