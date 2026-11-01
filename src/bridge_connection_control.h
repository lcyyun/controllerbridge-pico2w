#pragma once
#include <cstdint>

void bridge_connection_apply(bool user_requested = false);
void bridge_connection_reset(const char *reason);
bool bridge_connection_source_selected(uint8_t source);
