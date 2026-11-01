#pragma once
#include <cstdint>
using absolute_time_t = uint64_t;
uint64_t time_us_64();
inline absolute_time_t get_absolute_time() { return time_us_64(); }
inline int64_t absolute_time_diff_us(absolute_time_t a, absolute_time_t b) { return static_cast<int64_t>(b-a); }
