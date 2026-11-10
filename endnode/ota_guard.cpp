#include "ota_guard.h"

#include "certs/FirmwareUpdatePublicKey.h"
#include "logger.h"
#include "wake_policy.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <string.h>

static int ota_b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int ota_b64_decode(const char* in, uint8_t* out, size_t out_len) {
    int value = 0;
    int bits = -8;
    size_t n = 0;
    if (!in || !out) return -1;
    for (const char* p = in; *p; p++) {
        if (*p == '=' || *p == '\r' || *p == '\n' || *p == ' ') continue;
        int digit = ota_b64_value(*p);
        if (digit < 0) return -1;
        value = (value << 6) + digit;
        bits += 6;
        if (bits >= 0) {
            if (n >= out_len) return -1;
            out[n++] = (uint8_t)((value >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return (int)n;
}

bool ota_signature_valid(const uint8_t hash[32], const char* signature_b64) {
    if (!hash || !signature_b64 || !signature_b64[0]) {
        local_logger("OTA rejected. SIGNATURE is missing.");
        return false;
    }
    uint8_t sig[96];
    int sig_len = ota_b64_decode(signature_b64, sig, sizeof(sig));
    if (sig_len <= 0) {
        local_logger("OTA rejected. SIGNATURE is not valid base64.");
        return false;
    }
    char pem[384];
    size_t n = 0;
    const char* src = FirmwareUpdatePublicKey;
    while (src[n] && n + 1 < sizeof(pem)) {
        pem[n] = src[n];
        n++;
    }
    pem[n] = '\0';

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int parsed = mbedtls_pk_parse_public_key(&pk, reinterpret_cast<const unsigned char*>(pem), n + 1);
    if (parsed != 0) {
        local_logger("OTA rejected. Signing public key could not be parsed: ", parsed);
        mbedtls_pk_free(&pk);
        return false;
    }
    int verified = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, (size_t)sig_len);
    mbedtls_pk_free(&pk);
    if (verified != 0) {
        local_logger("OTA rejected. ECDSA P-256 signature is invalid: ", verified);
        return false;
    }
    return true;
}

void ota_remember_slot() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    Preferences prefs;
    if (!prefs.begin("otarol", false) || !running || !running->label) return;
    prefs.putString("prev", running->label);
    prefs.putUChar("tries", 0);
    prefs.putUChar("conf", 0);
    prefs.putUChar("ok", 0);
    prefs.putUChar("miss", 0);
    prefs.end();
}

// espressif32 6.7.0 ships the Arduino 2.0.16 ESP32-S3 bootloader with
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE. That rollback fires on the next
// reset while the image is still PENDING_VERIFY. Deep sleep is a reset, so
// the bootloader would revert this image on the first sleep, before two
// successful sleep uplinks or six missed wakes. Arduino also marks the image
// valid in initArduino() unless verifyRollbackLater() is overridden.
// The 2-uplink / 6-miss rule therefore uses this app-level otarol record
// and esp_ota_set_boot_partition, not the bootloader rollback.
void ota_note_boot() {
    // Boot count is not the rollback trigger. ota_account_uplink() is.
}

extern uint8_t radio_skip_remaining;

static OtaConfirmAccount ota_load_account() {
    OtaConfirmAccount state;
    memset(&state, 0, sizeof(state));
    Preferences prefs;
    if (!prefs.begin("otarol", true)) return state;
    state.confirmed = prefs.getUChar("conf", 0);
    state.deep_sleep_uplinks = prefs.getUChar("ok", 0);
    state.wakes_without_uplink = prefs.getUChar("miss", 0);
    prefs.end();
    return state;
}

static void ota_store_account(const OtaConfirmAccount& state) {
    Preferences prefs;
    if (!prefs.begin("otarol", false)) return;
    prefs.putUChar("conf", state.confirmed);
    prefs.putUChar("ok", state.deep_sleep_uplinks);
    prefs.putUChar("miss", state.wakes_without_uplink);
    prefs.end();
}

static void ota_boot_previous_slot() {
    Preferences prefs;
    if (!prefs.begin("otarol", false)) return;
    String prev = prefs.getString("prev", "");
    prefs.putUChar("miss", 0);
    prefs.putUChar("tries", 0);
    prefs.end();
    if (prev.length() == 0) return;
    const esp_partition_t* previous = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, prev.c_str());
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (previous && running && previous != running) {
        local_logger("Unconfirmed image missed 6 uplinks. Booting the previous slot.");
        radio_skip_clear(&radio_skip_remaining);
        esp_ota_set_boot_partition(previous);
        ESP.restart();
    }
}

void ota_confirm_image() {
    Preferences prefs;
    if (!prefs.begin("otarol", false)) return;
    prefs.putUChar("tries", 0);
    prefs.putUChar("conf", 1);
    prefs.putUChar("miss", 0);
    prefs.end();
}

void ota_account_uplink(bool first_run, bool uplink_ok) {
    OtaConfirmAccount state = ota_load_account();
    OtaConfirmKind kind = account_ota_confirm(&state, first_run, uplink_ok);
    ota_store_account(state);
    if (kind == OTA_CONFIRM_MARK) {
        local_logger("Two deep-sleep uplinks succeeded. Confirming this image.");
        ota_confirm_image();
        return;
    }
    if (kind == OTA_CONFIRM_ROLLBACK) ota_boot_previous_slot();
}
