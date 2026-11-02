#pragma once

#include <stdint.h>

// Clamps for backend parameters. TTS_SEC, R_RATE and C_F_UPDATE must never be 0.
static const uint32_t TTS_SEC_MIN = 60;
static const uint32_t TTS_SEC_MAX = 86400;
static const uint32_t R_RATE_MIN = 1;
static const uint32_t R_RATE_MAX = 3;  // 4 readings overflow the RTC pool
static const uint32_t C_F_UPDATE_MIN_MS = 3600000UL;
static const uint32_t C_F_UPDATE_MAX_MS = 604800000UL;
static const uint32_t TTR_SENSOR_MIN_MS = 1000;
static const uint32_t TTR_SENSOR_MAX_MS = 120000;
static const uint32_t TB_READINGS_MAX_SEC = 120;
static const uint32_t R_COUNT_MIN = 1;
static const uint32_t R_COUNT_MAX = 12;
static const unsigned long AWAKE_BUDGET_MS = 240000UL;
static const unsigned long NET_PHASE_BUDGET_MS = 90000UL;
static const unsigned long MQTT_PHASE_BUDGET_MS = 40000UL;
static const unsigned long HTTPS_SIZE_BUDGET_MS = 20000UL;

inline uint32_t clamp_u32(uint32_t value, uint32_t lo, uint32_t hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

inline uint32_t clamp_i32_to_u32(long value, uint32_t lo, uint32_t hi) {
    if (value < (long)lo) return lo;
    if (value > (long)hi) return hi;
    return (uint32_t)value;
}
