#pragma once

#include <stdint.h>

// Pack-voltage policy for a 3S pack with a 7.5 V BMS cutoff.
// Thresholds are volts. The caller supplies a clamped copy of the backend values.

static const float POWER_LOW_ENTER_V = 10.8f;
static const float POWER_LOW_EXIT_V = 11.4f;
static const float POWER_CRITICAL_V = 9.9f;
static const uint32_t POWER_LOW_MIN_SLEEP_SEC = 1800;
static const uint32_t POWER_CRITICAL_MIN_SLEEP_SEC = 3600;

enum PowerMode {
    POWER_NORMAL = 0,
    POWER_LOW = 1,
    POWER_CRITICAL = 2
};

struct PowerInput {
    float pack_volts;
    bool pack_valid;
    PowerMode previous;
    uint32_t backend_sleep_sec;
    uint32_t backend_reading_count;
    bool force_tts;
    uint32_t force_sleep_sec;
    bool force_active;
};

struct PowerDecision {
    PowerMode mode;
    uint32_t eff_sleep_sec;
    uint32_t reading_count;
    bool radio_allowed;
    bool changed;
};

inline PowerMode power_mode_from_voltage(float volts, PowerMode previous, float low_enter, float low_exit, float critical) {
    if (volts <= critical) return POWER_CRITICAL;
    if (previous == POWER_CRITICAL && volts < low_exit) return POWER_LOW;
    if (previous == POWER_LOW) {
        if (volts >= low_exit) return POWER_NORMAL;
        if (volts <= critical) return POWER_CRITICAL;
        return POWER_LOW;
    }
    if (previous == POWER_NORMAL && volts <= low_enter) return POWER_LOW;
    if (volts <= low_enter) return POWER_LOW;
    return POWER_NORMAL;
}

// Precedence: critical > FORCE_TTS (while active) > tier minimum > backend value.
inline PowerDecision decide_power(const PowerInput& in) {
    PowerDecision out;
    out.mode = in.previous;
    out.reading_count = in.backend_reading_count == 0 ? 1 : in.backend_reading_count;
    out.radio_allowed = true;
    out.changed = false;
    out.eff_sleep_sec = in.backend_sleep_sec == 0 ? 60 : in.backend_sleep_sec;

    if (!in.pack_valid) {
        if (in.force_active && in.force_tts && in.force_sleep_sec > 0) {
            out.eff_sleep_sec = in.force_sleep_sec;
        }
        return out;
    }

    out.mode = power_mode_from_voltage(in.pack_volts, in.previous, POWER_LOW_ENTER_V, POWER_LOW_EXIT_V, POWER_CRITICAL_V);
    out.changed = out.mode != in.previous;

    if (out.mode == POWER_CRITICAL) {
        out.radio_allowed = false;
        uint32_t floor_sec = POWER_CRITICAL_MIN_SLEEP_SEC;
        out.eff_sleep_sec = in.backend_sleep_sec > floor_sec ? in.backend_sleep_sec : floor_sec;
        if (out.reading_count < 4) out.reading_count = 4;
        return out;
    }

    if (in.force_active && in.force_tts && in.force_sleep_sec > 0) {
        out.eff_sleep_sec = in.force_sleep_sec;
        return out;
    }

    if (out.mode == POWER_LOW) {
        out.eff_sleep_sec = in.backend_sleep_sec > POWER_LOW_MIN_SLEEP_SEC ? in.backend_sleep_sec : POWER_LOW_MIN_SLEEP_SEC;
        if (out.reading_count > 1) out.reading_count = 1;
        return out;
    }

    out.eff_sleep_sec = in.backend_sleep_sec;
    return out;
}
