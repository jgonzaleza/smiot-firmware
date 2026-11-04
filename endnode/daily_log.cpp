#include "daily_log.h"

#include "app_utils.h"
#include "firmware_context.h"
#include "time_manager.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <stdio.h>

RTC_DATA_ATTR DailyHealth g_daily_health = {
    -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1
};

RTC_DATA_ATTR int g_log_level = LOG_INFO;

static int32_t health_today() {
    uint32_t now = (uint32_t)get_current_timestamp();
    if (now == 0) return -1;
    return local_day_number(now, (int32_t)timezone_offset_sec);
}

static void health_touch_today() {
    int32_t today = health_today();
    if (today >= 0) health_ensure_day(&g_daily_health, today);
}

void daily_log_boot(int reset_reason) {
    health_touch_today();
    health_count_reset(&g_daily_health, reset_reason);
}

void daily_log_note_net_fail() {
    health_touch_today();
    if (g_daily_health.net_fail < 65535) g_daily_health.net_fail++;
}

void daily_log_note_gps_fail() {
    health_touch_today();
    if (g_daily_health.gps_fail < 65535) g_daily_health.gps_fail++;
}

void daily_log_note_sd_error() {
    health_touch_today();
    if (g_daily_health.sd_err < 65535) g_daily_health.sd_err++;
}

void daily_log_note_voltage(float volts) {
    if (!(volts > 5.0f && volts < 20.0f)) return;
    health_touch_today();
    int centi = (int)(volts * 100.0f + (volts >= 0 ? 0.5f : -0.5f));
    health_note_voltage_centi(&g_daily_health, centi);
}

void daily_log_set_level_code(int level) {
    if (level < LOG_ERROR || level > LOG_DEBUG) return;
    g_log_level = level;
}

void daily_log_set_level_text(const char* text) {
    int level = parse_log_level(text, -1);
    if (level < 0) return;
    g_log_level = level;
    preferences.begin("devcfg", false);
    preferences.putUChar("loglvl", (uint8_t)g_log_level);
    preferences.end();
}

int daily_log_level() {
    return g_log_level;
}

static int rtc_queue_records() {
    int depth = 0;
    if (rtc_pool_length <= 0) return 0;
    int limit = rtc_pool_length;
    if (limit > RTC_POOL_BYTES) limit = RTC_POOL_BYTES;
    for (int i = 0; i < limit; i++) {
        if (rtc_payload_pool[i] == '\n') depth++;
    }
    return depth;
}

static int sd_queue_records() {
    if (!is_sdcard_ok || !SD.exists(QUEUE_FILE)) return 0;
    File file = SD.open(QUEUE_FILE, FILE_READ);
    if (!file) return 0;
    int depth = 0;
    size_t seen = 0;
    while (file.available() && depth < 2000 && seen < (512UL * 1024UL)) {
        int byte = file.read();
        seen++;
        if (byte == '\n') depth++;
        if ((seen & 0x3FF) == 0) feed_watchdog();
    }
    file.close();
    return depth;
}

static void prune_daily_logs(int32_t today) {
    File dir = SD.open("/log");
    if (!dir) return;
    File entry = dir.openNextFile();
    while (entry) {
        String name = entry.name();
        entry.close();
        int32_t file_day = -1;
        if (log_name_to_day(name.c_str(), &file_day) && log_file_expired(file_day, today)) {
            String path = name.startsWith("/") ? name : String("/log/") + name;
            SD.remove(path);
        }
        entry = dir.openNextFile();
        feed_watchdog();
    }
    dir.close();
}

static void append_centi(JsonObject health, const char* key, int centi) {
    if (centi < 0) return;
    char text[8];
    int whole = centi / 100;
    int frac = centi % 100;
    if (frac < 0) frac = -frac;
    snprintf(text, sizeof(text), "%d.%02d", whole, frac);
    health[key] = text;
}

void attach_daily_health(JsonDocument& doc) {
    int32_t today = health_today();
    if (today >= 0) health_ensure_day(&g_daily_health, today);
    if (!health_due(&g_daily_health, today, active_power_mode == POWER_NORMAL)) return;

    uint32_t elapsed = 0;
    if (g_wake_start_ms > 0 && millis() >= g_wake_start_ms) {
        elapsed = (millis() - g_wake_start_ms) / 1000UL;
    }
    char day_text[9];
    format_log_day(g_daily_health.day >= 0 ? g_daily_health.day : today, day_text);

    JsonObject health = doc["health"].to<JsonObject>();
    health["day"] = day_text;
    health["rst_wdt"] = g_daily_health.rst_wdt;
    health["rst_panic"] = g_daily_health.rst_panic;
    health["rst_brown"] = g_daily_health.rst_brown;
    health["rst_sw"] = g_daily_health.rst_sw;
    health["rst_other"] = g_daily_health.rst_other;
    health["wdt"] = g_daily_health.wdt;
    health["nf"] = g_daily_health.net_fail;
    health["gf"] = g_daily_health.gps_fail;
    health["sd_err"] = g_daily_health.sd_err;
    health["q"] = rtc_queue_records() + sd_queue_records();
    append_centi(health, "vmin", g_daily_health.vmin_centi);
    append_centi(health, "vmax", g_daily_health.vmax_centi);
    health["awake"] = g_daily_health.awake_sec + elapsed;
    health["rssi"] = last_rssi;
    health["sd"] = is_sdcard_ok ? 1 : 0;
    health["iccid"] = sim_iccid;
    health["idx"] = current_network_index;
    g_daily_health.sent_day = today;
}

void write_daily_wake_log() {
    int32_t today = health_today();
    if (today >= 0) health_ensure_day(&g_daily_health, today);

    bool error = !is_sdcard_ok || g_daily_health.net_fail > 0 || g_daily_health.gps_fail > 0 ||
        g_daily_health.sd_err > 0 || g_daily_health.wdt > 0 || g_daily_health.rst_panic > 0 ||
        active_power_mode == POWER_CRITICAL;
    bool warn = active_power_mode == POWER_LOW || (last_rssi >= 0 && last_rssi < 8);
    uint32_t elapsed = 0;
    if (g_wake_start_ms > 0 && millis() >= g_wake_start_ms) elapsed = (millis() - g_wake_start_ms) / 1000UL;
    if (!wake_log_enabled(g_log_level, error, warn)) {
        health_add_awake(&g_daily_health, elapsed);
        return;
    }
    if (!ensure_sd_active()) {
        health_add_awake(&g_daily_health, elapsed);
        return;
    }

    WakeLogFields fields;
    memset(&fields, 0, sizeof(fields));
    fields.level = g_log_level;
    fields.unix_time = (uint32_t)get_current_timestamp();
    fields.sim = sim_label.c_str();
    fields.iccid = sim_iccid.c_str();
    fields.plmn = registered_plmn.c_str();
    fields.carrier_index = current_network_index;
    fields.rssi = last_rssi;
    fields.vmin_centi = g_daily_health.vmin_centi;
    fields.vmax_centi = g_daily_health.vmax_centi;
    fields.sd_present = is_sdcard_ok ? 1 : 0;
    fields.queue_depth = rtc_queue_records();
    fields.net_fail = g_daily_health.net_fail;
    fields.gps_fail = g_daily_health.gps_fail;
    fields.sd_err = g_daily_health.sd_err;
    fields.awake_sec = g_daily_health.awake_sec + elapsed;

    char line[192];
    if (format_wake_log(&fields, line, sizeof(line)) < 0) {
        add_sd_error("[DAILY_LOG_FORMAT]");
        health_add_awake(&g_daily_health, elapsed);
        return;
    }
    if (!SD.exists("/log")) SD.mkdir("/log");
    if (today >= 0) prune_daily_logs(today);

    char path[24];
    if (today >= 0) {
        char ymd[9];
        format_log_day(today, ymd);
        snprintf(path, sizeof(path), "/log/%s.log", ymd);
    } else {
        snprintf(path, sizeof(path), "/log/00000000.log");
    }
    File file = SD.open(path, FILE_APPEND);
    if (!file) {
        add_sd_error("[DAILY_LOG_OPEN]");
        health_add_awake(&g_daily_health, elapsed);
        return;
    }
    if (!file.println(line)) add_sd_error("[DAILY_LOG_WRITE]");
    file.close();
    health_add_awake(&g_daily_health, elapsed);
}
