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
// v3.0.0 (tag v3.0.0): TIME_TO_SLEEP_SEC is seconds. TIME_TO_READ_SENSOR,
// TIME_BETWEEN_READINGS, and MAX_WAIT_MS are milliseconds. wait_for_sensors
// compares the stabilize value to millis(), and the between-readings wait is
// delay(TIME_BETWEEN_READINGS), so 5000 is 5s and 2000 is 2s. Do not clamp a
// millisecond value against a seconds limit. The old TB_READINGS_MAX_SEC of
// 120 turned a configured 2000 into a 120s wait on every reading pass.
static const uint32_t MAX_WAIT_DEFAULT_MS = 60000;
static const uint32_t MAX_WAIT_MIN_MS = 1000;
static const uint32_t MAX_WAIT_MAX_MS = 180000;
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

// Firmware defaults, used until a backend payload replaces a field.
// v3.0.0 assigns these to the RTC variables at startup, then a payload
// overwrites only the fields it carries. Sleep is seconds. The sensor
// timings are milliseconds.
static const uint32_t DEFAULT_SLEEP_SEC = 300;
static const uint32_t DEFAULT_TIME_TO_READ_SENSOR = 5000;
static const uint32_t DEFAULT_TIME_BETWEEN_READINGS = 2000;
static const uint32_t DEFAULT_READING_COUNT = 1;
static const uint32_t DEFAULT_READING_RATE = 1;

struct DeviceTiming {
    uint32_t sleep_sec;
    uint32_t stabilize_ms;
    uint32_t between_ms;
    uint32_t reading_count;
    uint32_t reading_rate;
};

struct BackendTiming {
    bool has_sleep;
    long sleep_sec;
    bool has_stabilize;
    long stabilize_ms;
    bool has_between;
    long between_ms;
    bool has_count;
    long reading_count;
    bool has_rate;
    long reading_rate;
};

inline DeviceTiming firmware_timing_defaults() {
    DeviceTiming timing = {};
    timing.sleep_sec = DEFAULT_SLEEP_SEC;
    timing.stabilize_ms = DEFAULT_TIME_TO_READ_SENSOR;
    timing.between_ms = DEFAULT_TIME_BETWEEN_READINGS;
    timing.reading_count = DEFAULT_READING_COUNT;
    timing.reading_rate = DEFAULT_READING_RATE;
    return timing;
}

// Seconds. 300 stays 300.
inline uint32_t configured_sleep_sec(long value) {
    return clamp_i32_to_u32(value, TTS_SEC_MIN, TTS_SEC_MAX);
}

// Milliseconds, compared with millis(). Bounds are in milliseconds, so
// 5000 stays 5000. A seconds cap of 120 must not see this number.
inline uint32_t configured_stabilize_ms(long value) {
    return clamp_i32_to_u32(value, TTR_SENSOR_MIN_MS, TTR_SENSOR_MAX_MS);
}

// Milliseconds, passed to delay() the way v3.0.0 does. No upper clamp:
// 2000 waits 2000 ms, not 2000 s and not 120 s.
inline uint32_t configured_between_ms(long value) {
    if (value <= 0) return 0;
    return (uint32_t)value;
}

// A present field replaces the current value. A missing field keeps it.
// Millisecond fields are not passed through a seconds cap.
inline DeviceTiming apply_backend_timing(DeviceTiming current, const BackendTiming& incoming) {
    if (incoming.has_sleep) current.sleep_sec = configured_sleep_sec(incoming.sleep_sec);
    if (incoming.has_stabilize) current.stabilize_ms = configured_stabilize_ms(incoming.stabilize_ms);
    if (incoming.has_between) current.between_ms = configured_between_ms(incoming.between_ms);
    if (incoming.has_count) {
        current.reading_count = clamp_i32_to_u32(incoming.reading_count, R_COUNT_MIN, R_COUNT_MAX);
    }
    if (incoming.has_rate) {
        current.reading_rate = clamp_i32_to_u32(incoming.reading_rate, R_RATE_MIN, R_RATE_MAX);
    }
    return current;
}

// Milliseconds. A missing value keeps the v3 default of 60000. Bounds are
// milliseconds, so 2000 and 5000 are unchanged.
inline uint32_t configured_max_wait_ms(long value, bool present) {
    if (!present) return MAX_WAIT_DEFAULT_MS;
    return clamp_i32_to_u32(value, MAX_WAIT_MIN_MS, MAX_WAIT_MAX_MS);
}
