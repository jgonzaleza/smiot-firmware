#include "ota_guard.h"

#include "certs/FirmwareUpdatePublicKey.h"
#include "logger.h"

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
    prefs.end();
}

void ota_note_boot() {
    Preferences prefs;
    if (!prefs.begin("otarol", false)) return;
    uint8_t tries = prefs.getUChar("tries", 0);
    String prev = prefs.getString("prev", "");
    if (tries >= 3 && prev.length() > 0) {
        const esp_partition_t* previous = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, prev.c_str());
        const esp_partition_t* running = esp_ota_get_running_partition();
        prefs.putUChar("tries", 0);
        prefs.end();
        if (previous && running && previous != running) {
            esp_ota_set_boot_partition(previous);
            ESP.restart();
        }
        return;
    }
    prefs.putUChar("tries", (uint8_t)(tries + 1));
    prefs.end();
}

void ota_confirm_image() {
    Preferences prefs;
    if (!prefs.begin("otarol", false)) return;
    prefs.putUChar("tries", 0);
    prefs.end();
}
