#pragma once

#include <stddef.h>
#include <stdint.h>

// OTA offer check with no crypto library. This does not verify a signature.
// S3_CONFIG.SHA256 is 64 hex characters (SHA-256 of the image bytes).
// S3_CONFIG.SIGNATURE is base64 of the DER ECDSA P-256 signature over those bytes.
// OTA_PRECHECK_NEED_SIGNATURE means the hash matched and a signature string is
// present. The device must still run ECDSA before Update.end(). A host test
// must not treat that value as a successful verify.

enum OtaPrecheck {
    OTA_PRECHECK_REJECT_MISSING = 1,
    OTA_PRECHECK_REJECT_HASH = 2,
    OTA_PRECHECK_NEED_SIGNATURE = 3
};

inline bool ota_hex_ci_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'F') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'F') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

inline bool ota_sha256_hex_ok(const char* hex) {
    if (!hex) return false;
    int n = 0;
    for (; hex[n]; n++) {
        char c = hex[n];
        bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!digit) return false;
    }
    return n == 64;
}

inline void sha256_to_hex(const uint8_t hash[32], char out[65]) {
    static const char* hexd = "0123456789abcdef";
    if (!hash || !out) return;
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hexd[hash[i] >> 4];
        out[i * 2 + 1] = hexd[hash[i] & 0x0f];
    }
    out[64] = '\0';
}

inline OtaPrecheck ota_precheck(const char* offered_sha_hex, const char* signature_b64, const char* actual_sha_hex) {
    if (!offered_sha_hex || !offered_sha_hex[0] || !signature_b64 || !signature_b64[0]) {
        return OTA_PRECHECK_REJECT_MISSING;
    }
    if (!ota_sha256_hex_ok(offered_sha_hex) || !ota_sha256_hex_ok(actual_sha_hex) ||
        !ota_hex_ci_equal(offered_sha_hex, actual_sha_hex)) {
        return OTA_PRECHECK_REJECT_HASH;
    }
    return OTA_PRECHECK_NEED_SIGNATURE;
}
