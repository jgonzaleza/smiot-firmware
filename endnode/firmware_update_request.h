#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// -DCHECK_FIRMWARE_UPDATE_URL=\"https://...\" overrides this.
#ifndef CHECK_FIRMWARE_UPDATE_URL
#define CHECK_FIRMWARE_UPDATE_URL "https://junkzqt2yc.execute-api.us-east-1.amazonaws.com/pro/device_firmware_config"
#endif

// POST body for check_firmware_update(). client_id and variant are copied as-is.
// Returns the length without the trailing NUL, or -1 when the body does not fit.
inline int firmware_update_request_body(
    char* out,
    size_t out_len,
    const char* client_id,
    int device_type,
    const char* variant,
    int firmware_version,
    uint64_t device_id) {
    if (!out || out_len == 0 || !client_id || !variant) {
        return -1;
    }
    int written = snprintf(
        out,
        out_len,
        "{\"client_id\": \"%s\", \"device_type\":%d, \"variant\":\"%s\", \"firmware_version\":%d, \"device_id\": \"%llu\"}",
        client_id,
        device_type,
        variant,
        firmware_version,
        (unsigned long long)device_id);
    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return -1;
    }
    return written;
}
