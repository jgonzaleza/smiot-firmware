#include "rtc_persist.h"

#include <string.h>

RTC_NOINIT_ATTR RtcPersist g_rtc;

static bool g_loaded_valid = false;
static esp_reset_reason_t g_reset_reason = ESP_RST_UNKNOWN;

static uint32_t crc32_ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t persist_crc_now() {
    const uint8_t* start = reinterpret_cast<const uint8_t*>(&g_rtc.panic_flag);
    const size_t len = sizeof(RtcPersist) - offsetof(RtcPersist, panic_flag);
    return crc32_ieee(start, len);
}

void rtc_persist_commit() {
    g_rtc.magic = RTC_PERSIST_MAGIC;
    if (g_rtc.pool_length < 0 || g_rtc.pool_length >= RTC_POOL_BYTES) {
        g_rtc.pool_length = 0;
        g_rtc.pool[0] = '\0';
    } else {
        g_rtc.pool[g_rtc.pool_length] = '\0';
    }
    if (g_rtc.err_count > RTC_ERR_SLOTS) g_rtc.err_count = 0;
    if (g_rtc.err_head >= RTC_ERR_SLOTS) g_rtc.err_head = 0;
    g_rtc.crc = persist_crc_now();
}

static void persist_reset_defaults() {
    memset(&g_rtc, 0, sizeof(g_rtc));
    g_rtc.magic = RTC_PERSIST_MAGIC;
    g_rtc.is_first_run = 1;
    g_rtc.last_reset_day = -1;
    g_rtc.pool[0] = '\0';
    rtc_persist_commit();
}

void rtc_persist_boot() {
    g_reset_reason = esp_reset_reason();
    const bool magic_ok = (g_rtc.magic == RTC_PERSIST_MAGIC);
    const bool crc_ok = magic_ok && (g_rtc.crc == persist_crc_now());
    const bool bounds_ok = crc_ok
        && g_rtc.pool_length >= 0
        && g_rtc.pool_length < RTC_POOL_BYTES
        && g_rtc.err_count <= RTC_ERR_SLOTS
        && g_rtc.err_head < RTC_ERR_SLOTS;
    if (!bounds_ok) {
        persist_reset_defaults();
        g_loaded_valid = false;
        return;
    }
    g_rtc.pool[g_rtc.pool_length] = '\0';
    g_loaded_valid = true;
}

bool rtc_persist_was_valid() {
    return g_loaded_valid;
}

bool rtc_reset_was_panic() {
    if (g_rtc.panic_flag == 1) return true;
    switch (g_reset_reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return true;
        default:
            return false;
    }
}

uint32_t payload_hash32(const uint8_t* data, uint32_t len) {
    uint32_t hash = 2166136261u;
    if (!data) return 1;
    for (uint32_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    if (hash == 0) hash = 1;
    return hash;
}

void rtc_error_ring_push(const char* msg) {
    if (!msg || !msg[0]) return;
    if (g_rtc.magic != RTC_PERSIST_MAGIC) return;
    char* slot = g_rtc.err_ring[g_rtc.err_head];
    memset(slot, 0, RTC_ERR_BYTES);
    strncpy(slot, msg, RTC_ERR_BYTES - 1);
    g_rtc.err_head = (uint8_t)((g_rtc.err_head + 1) % RTC_ERR_SLOTS);
    if (g_rtc.err_count < RTC_ERR_SLOTS) g_rtc.err_count++;
    rtc_persist_commit();
}

int rtc_error_ring_format(char* out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    for (uint8_t n = 0; n < g_rtc.err_count; n++) {
        uint8_t idx = (uint8_t)((g_rtc.err_head + RTC_ERR_SLOTS - g_rtc.err_count + n) % RTC_ERR_SLOTS);
        const char* slot = g_rtc.err_ring[idx];
        size_t slen = strnlen(slot, RTC_ERR_BYTES - 1);
        if (slen == 0) continue;
        size_t need = slen + (used > 0 ? 1 : 0);
        if (used + need + 1 > out_len) break;
        if (used > 0) out[used++] = ';';
        memcpy(out + used, slot, slen);
        used += slen;
        out[used] = '\0';
    }
    return (int)used;
}

void rtc_error_ring_clear() {
    memset(g_rtc.err_ring, 0, sizeof(g_rtc.err_ring));
    g_rtc.err_count = 0;
    g_rtc.err_head = 0;
    rtc_persist_commit();
}
