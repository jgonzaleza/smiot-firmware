#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include <stddef.h>
#include <stdint.h>

// RTC slow memory that survives deep sleep AND software / panic resets.
// A magic and CRC reject power-on garbage. The pool is 6144 bytes so the
// 16x24 error ring fits beside the other RTC_DATA_ATTR state.

static const uint32_t RTC_PERSIST_MAGIC = 0x53494631UL;  // 'SIF1'
static const int RTC_POOL_BYTES = 6144;
static const int RTC_ERR_SLOTS = 16;
static const int RTC_ERR_BYTES = 24;

struct RtcPersist {
    uint32_t magic;
    uint32_t crc;
    int32_t panic_flag;
    int32_t upload_failures;
    int32_t network_changes;
    int32_t null_errors;
    int32_t network_index;
    int32_t reading_cnt;
    int32_t last_reset_day;
    uint32_t param_hash;
    uint32_t last_restart_id;
    int32_t pool_length;
    uint8_t is_first_run;
    uint8_t err_count;
    uint8_t err_head;
    uint8_t pad;
    char err_ring[RTC_ERR_SLOTS][RTC_ERR_BYTES];
    char pool[RTC_POOL_BYTES];
};

extern RTC_NOINIT_ATTR RtcPersist g_rtc;

// Names used throughout the sketch. The preprocessor does not re-expand the
// macro inside its own replacement, so g_rtc.first_run stays a field access.
#define rtc_panic_flag (g_rtc.panic_flag)
#define consecutive_upload_failures (g_rtc.upload_failures)
#define consecutive_network_changes (g_rtc.network_changes)
#define consecutive_null_errors (g_rtc.null_errors)
#define current_network_index (g_rtc.network_index)
#define readingCnt (g_rtc.reading_cnt)
#define lastResetDayNumber (g_rtc.last_reset_day)
#define first_run (g_rtc.is_first_run)
#define rtc_payload_pool (g_rtc.pool)
#define rtc_pool_length (g_rtc.pool_length)

void rtc_persist_boot();
void rtc_persist_commit();
bool rtc_persist_was_valid();
bool rtc_reset_was_panic();
uint32_t payload_hash32(const uint8_t* data, uint32_t len);
void rtc_error_ring_push(const char* msg);
int rtc_error_ring_format(char* out, size_t out_len);
void rtc_error_ring_clear();
