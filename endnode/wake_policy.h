#pragma once

#include <stdint.h>

// Host-testable wake decisions. No Arduino types.
//
// ESP_RST_DEEPSLEEP is 8 (esp_reset_reason_t). A software restart is 3.
// Deep-sleep modem bring-up keys off the reset reason only. The last
// sleep wakeup cause can still read as the timer after esp_restart(),
// and that must stay on the cold-boot path.

static const int MODEM_RESET_REASON_DEEPSLEEP = 8;
static const uint8_t RADIO_SKIP_CAP = 3;
static const uint32_t UPLINK_STALE_SEC = 7200;
static const uint8_t OTA_CONFIRM_UPLINKS = 2;
static const uint8_t OTA_ROLLBACK_MISSES = 6;

inline bool modem_wake_from_deep_sleep(int reset_reason) {
    return reset_reason == MODEM_RESET_REASON_DEEPSLEEP;
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
