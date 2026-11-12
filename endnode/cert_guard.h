#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// FNV-1a over the compiled client certificate and private key.
// A change means the modem copies are stale and must be replaced once.
inline uint32_t cert_material_hash(const char* cert, const char* key) {
    uint32_t hash = 2166136261u;
    const char* parts[2] = {cert ? cert : "", key ? key : ""};
    for (int part = 0; part < 2; part++) {
        hash ^= (uint32_t)(part + 1);
        hash *= 16777619u;
        for (const unsigned char* byte = (const unsigned char*)parts[part]; *byte; byte++) {
            hash ^= *byte;
            hash *= 16777619u;
        }
    }
    return hash;
}

// SIMCom +CMQTTCONNECT TLS failures. 34 is "open session failed".
inline bool cert_tls_error(int code) {
    return code == 32 || code == 33 || code == 34;
}

// Token for health last_net_err and the SD wake log. "cmqtt_34" fits in 24 bytes.
inline int format_mqtt_net_err(int code, char* out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    int written = snprintf(out, out_len, "cmqtt_%d", code);
    if (written < 0 || (size_t)written >= out_len) return -1;
    return written;
}

void cert_guard_boot_warning();
