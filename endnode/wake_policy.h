#pragma once

#include <stdint.h>

// Host-testable wake decisions. No Arduino types.
//
// ESP_RST_DEEPSLEEP is 8 (esp_reset_reason_t). A software restart is 3.
// The v3.0.0 modem sequence does not branch on this. The reset reason
// is still recorded so a software restart is not mistaken for deep sleep.

static const int MODEM_RESET_REASON_DEEPSLEEP = 8;
static const uint8_t RADIO_SKIP_CAP = 3;
static const uint32_t UPLINK_STALE_SEC = 7200;
static const uint8_t OTA_CONFIRM_UPLINKS = 2;
static const uint8_t OTA_ROLLBACK_MISSES = 6;
// Kept for the CPIN parser tests. The v3.0.0 power sequence does not
// wait this long before the reset pulse.
static const uint32_t MODEM_SIM_READY_MS = 10000;
// v3.0.0 on-pulse. Ton >= 100 <= 500. Turns an A7608 on from off.
static const uint32_t MODEM_PWRKEY_ON_MS = 100;
// v3.0.0 retry press, used when AT stays down after the on-pulse.
static const uint32_t MODEM_PWRKEY_RETRY_MS = 1000;
// v3.0.0 go_sleep: up to 60 AT+CPOF tries, 500ms apart, then 5s, then
// up to 50 testAT() waits (TinyGSM's default timeout) 1s apart.
static const int MODEM_CPOF_TRIES = 60;
static const uint32_t MODEM_CPOF_GAP_MS = 500;
static const uint32_t MODEM_CPOF_SETTLE_MS = 5000;
static const int MODEM_CPOF_AT_TRIES = 50;
// v3.0.0 init_modem polls AT up to 50 times.
static const int MODEM_AT_PROBES = 50;
// v3.0.0 does not gpio_hold PWRKEY across sleep.
static const bool MODEM_PWRKEY_HOLD_IN_SLEEP = false;
// v3.0.0 keeps the sensor 5V rail and RS485 off through init_modem and
// the network step. The rail comes on only for the sensor read.
static const bool SENSOR_RAIL_DURING_MODEM = false;

// An AT read returns when its timeout elapses. Bytes still sitting in
// the UART do not extend that timeout.
inline bool at_read_timed_out(uint32_t elapsed_ms, uint32_t timeout_ms, bool bytes_waiting) {
    (void)bytes_waiting;
    return elapsed_ms >= timeout_ms;
}

// First-boot waits (install delay, NTP, GPS) stop at the awake budget.
inline uint32_t cap_wait_ms(uint32_t want_ms, uint32_t elapsed_ms, uint32_t budget_ms) {
    if (elapsed_ms >= budget_ms) return 0;
    uint32_t left = budget_ms - elapsed_ms;
    return want_ms < left ? want_ms : left;
}

inline bool modem_wake_from_deep_sleep(int reset_reason) {
    return reset_reason == MODEM_RESET_REASON_DEEPSLEEP;
}

// v3.0.0 slept the whole configured interval. Awake time is not
// subtracted, and a long wake does not shorten the next sleep.
inline uint32_t sleep_sec_v300(uint32_t interval_sec) {
    return interval_sec;
}

// After a wake, sleep at least this long. This is not the old 20s floor.
static const uint32_t MIN_SLEEP_AFTER_WAKE_SEC = 120;

// Sleep the configured interval. Time already awake is not subtracted,
// so the off time between wakes stays constant. An interval under 120s
// sleeps 120s. This is not the old 20s floor.
inline uint32_t sleep_sec_at_least(uint32_t awake_sec, uint32_t interval_sec) {
    (void)awake_sec;
    return interval_sec >= MIN_SLEEP_AFTER_WAKE_SEC ? interval_sec : MIN_SLEEP_AFTER_WAKE_SEC;
}

// v3.0.0 init_modem: `if (retry++ > 10)` then a 1000ms PWRKEY press.
// The press runs on the 12th failed AT probe, then the counter restarts.
inline bool v300_pwrkey_retry_due(int& retry) {
    return retry++ > 10;
}

inline const char* wake_text_find(const char* text, const char* needle) {
    if (!text || !needle || !needle[0]) return 0;
    for (const char* p = text; *p; ++p) {
        const char* a = p;
        const char* b = needle;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') return p;
    }
    return 0;
}

// True only for a +CPIN: line that says READY. "NOT READY" is not ready.
// "SMS READY" without +CPIN: is not a SIM status.
inline bool cpin_text_is_ready(const char* text) {
    const char* mark = wake_text_find(text, "+CPIN:");
    if (!mark) return false;
    mark += 6;
    const char* end = mark;
    while (*end && *end != '\r' && *end != '\n') ++end;
    unsigned nlen = 0;
    const char* not_ready = "NOT READY";
    while (not_ready[nlen]) ++nlen;
    for (const char* p = mark; p + nlen <= end; ++p) {
        unsigned i = 0;
        while (i < nlen && p[i] == not_ready[i]) ++i;
        if (i == nlen) return false;
    }
    return wake_text_find(mark, "READY") != 0 && wake_text_find(mark, "READY") < end;
}

// v3.0.0 getSimStatus matches "READY" before "NOT READY". endsWith("READY")
// is true for "NOT READY", so the network step continues. The token is the
// rest of the +CPIN: line. SIM PIN, SIM PUK, and NOT INSERTED do not match.
// "SMS READY" on a later line is not this token.
inline bool cpin_v3_continues(const char* text) {
    const char* mark = wake_text_find(text, "+CPIN:");
    if (!mark) return false;
    mark += 6;
    while (*mark == ' ' || *mark == '\t') ++mark;
    const char* end = mark;
    while (*end && *end != '\r' && *end != '\n') ++end;
    const char ready[] = "READY";
    unsigned n = 0;
    while (ready[n]) ++n;
    if ((unsigned)(end - mark) < n) return false;
    const char* tail = end - n;
    for (unsigned i = 0; i < n; ++i) {
        if (tail[i] != ready[i]) return false;
    }
    return true;
}

// CPIN predicate only. The v3.0.0 power-on sequence does not branch on it.
inline bool sim_not_ready_needs_cold_reset(bool at_answered, bool cpin_ready) {
    return at_answered && !cpin_ready;
}

// sim_ready is the wake trace: 1 ready, 0 not ready, -1 not reached.
// A SIM that never answered cannot register on another mode.
inline bool network_mode_retries_allowed(int sim_ready) {
    return sim_ready != 0;
}

struct RadioSkipAccount {
    uint8_t skip_remaining;
    int32_t consecutive_failures;
};

enum RadioAccountKind {
    RADIO_ACCOUNT_SUCCESS = 0,
    RADIO_ACCOUNT_FAILURE = 1,
    RADIO_ACCOUNT_SKIPPED = 2,
    RADIO_ACCOUNT_RESTART = 3
};

// True when this transmit wake must not touch the radio.
// Caps the skip at 3, then consumes one. Does not re-arm it.
// An announce wake and the post-OTA first_run wake still use the radio.
inline bool radio_skip_this_wake(uint8_t* skip_remaining, bool announce_power_mode, bool on_first_run) {
    if (!skip_remaining) return false;
    if (*skip_remaining > RADIO_SKIP_CAP) *skip_remaining = RADIO_SKIP_CAP;
    if (*skip_remaining > 0 && !announce_power_mode && !on_first_run) {
        (*skip_remaining)--;
        return true;
    }
    return false;
}

inline void radio_skip_clear(uint8_t* skip_remaining) {
    if (skip_remaining) *skip_remaining = 0;
}

// A skipped radio wake does not add a failure and does not arm another skip.
// A real failure arms a skip of exactly 3 once three failures have landed.
// Success clears both. Hitting max_failures zeroes the failure count and
// asks the caller to restart; the skip counter is left for perform_restart
// to clear, so a restart cannot inherit a fresh skip of 3.
inline RadioAccountKind account_radio_wake(RadioSkipAccount* state, bool sent, bool skipped, int max_failures) {
    if (!state) return RADIO_ACCOUNT_FAILURE;
    if (sent) {
        state->consecutive_failures = 0;
        state->skip_remaining = 0;
        return RADIO_ACCOUNT_SUCCESS;
    }
    if (skipped) return RADIO_ACCOUNT_SKIPPED;
    if (state->consecutive_failures < 1000000) state->consecutive_failures++;
    if (state->consecutive_failures >= 3) state->skip_remaining = RADIO_SKIP_CAP;
    if (max_failures > 0 && state->consecutive_failures >= max_failures) {
        state->consecutive_failures = 0;
        return RADIO_ACCOUNT_RESTART;
    }
    return RADIO_ACCOUNT_FAILURE;
}

// True when now is at least stale_sec after the last successful uplink.
// An unknown clock or a device that has never uplinked does not trip it.
inline bool uplink_absent_for(uint32_t now_unix, uint32_t last_success_unix, uint32_t stale_sec) {
    if (now_unix == 0 || last_success_unix == 0 || stale_sec == 0) return false;
    if (now_unix < last_success_unix) return false;
    return (now_unix - last_success_unix) >= stale_sec;
}

struct OtaConfirmAccount {
    uint8_t confirmed;
    uint8_t deep_sleep_uplinks;
    uint8_t wakes_without_uplink;
};

enum OtaConfirmKind {
    OTA_CONFIRM_NONE = 0,
    OTA_CONFIRM_MARK = 1,
    OTA_CONFIRM_ROLLBACK = 2
};

// The post-OTA first_run send does not confirm and does not count as one
// of the two deep-sleep uplinks. Two later successes confirm. Six transmit
// wakes with no uplink, while the image is still unconfirmed, roll back.
// A success clears the miss streak. A confirmed image stays idle.
inline OtaConfirmKind account_ota_confirm(OtaConfirmAccount* state, bool on_first_run, bool uplink_ok) {
    if (!state || state->confirmed) return OTA_CONFIRM_NONE;
    if (uplink_ok) {
        state->wakes_without_uplink = 0;
        if (on_first_run) return OTA_CONFIRM_NONE;
        if (state->deep_sleep_uplinks < 255) state->deep_sleep_uplinks++;
        if (state->deep_sleep_uplinks >= OTA_CONFIRM_UPLINKS) {
            state->confirmed = 1;
            return OTA_CONFIRM_MARK;
        }
        return OTA_CONFIRM_NONE;
    }
    if (state->wakes_without_uplink < 255) state->wakes_without_uplink++;
    if (state->wakes_without_uplink >= OTA_ROLLBACK_MISSES) return OTA_CONFIRM_ROLLBACK;
    return OTA_CONFIRM_NONE;
}
