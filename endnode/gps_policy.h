#pragma once

#include <stdint.h>

#include "sketch_config.h"

// Whether this wake reads the GNSS receiver or reuses a stored fix.
// No Arduino types. gps_sample() applies the result.
//
// Modem off: a valid cache, otherwise skip. The receiver is not powered.
// First boot with the modem already on: always try, even when G_GPS_DATA
// is off and L_LAT/L_LONG already filled the cache.
// Later wakes: G_GPS_DATA off plus a valid cache stays on that cache until
// gps_running_time reaches CHECK_GPS_UPDATE.

enum GpsSampleChoice {
    GPS_TRY = 0,
    GPS_CACHE = 1,
    GPS_SKIP = 2
};

struct GpsSampleInput {
    bool first_boot;
    bool modem_ready;
    bool get_gps_data;
    bool cache_ok;
    bool update_due;
};

inline GpsSampleChoice decide_gps_sample(const GpsSampleInput& in) {
    if (!in.modem_ready) {
        if (in.cache_ok) return GPS_CACHE;
        return GPS_SKIP;
    }
    if (in.first_boot) return GPS_TRY;
    if (in.update_due) return GPS_TRY;
    if (!in.get_gps_data && in.cache_ok) return GPS_CACHE;
    return GPS_TRY;
}

// How long this wake may poll AT+CGNSSINFO. A streak of misses is shorter
// than a cold start, and a cold start is longer than a warm check.
inline uint32_t gps_budget_sec(bool first_boot, bool cache_ok, uint8_t fail_streak) {
    if (fail_streak >= 3) return (uint32_t)GPS_FAIL_BUDGET_SEC;
    if (first_boot || !cache_ok) return (uint32_t)GPS_COLD_BUDGET_SEC;
    return (uint32_t)GPS_WARM_BUDGET_SEC;
}
