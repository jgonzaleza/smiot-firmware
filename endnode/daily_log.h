#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Daily SD log and the once-a-day health snapshot. No Arduino types.
//
// SD: one compact line per wake, file /log/YYYYMMDD.log, kept for 7 days.
// MQTT: a "health" object on the first normal-power transmit of the local day.
// Counters reset when that local day changes, so the object is this day so far
// (including earlier wakes that could not transmit). The SD file keeps every wake.

enum LogVerbosity {
    LOG_ERROR = 0,
    LOG_WARN = 1,
    LOG_INFO = 2,
    LOG_DEBUG = 3
};

struct DailyHealth {
    int32_t day;       // local day number, -1 until the clock is known
    int32_t sent_day;  // local day we already attached, -1 if never
    uint16_t rst_wdt;
    uint16_t rst_panic;
    uint16_t rst_brown;
    uint16_t rst_sw;
    uint16_t rst_other;
    uint16_t wdt;
    uint16_t net_fail;
    uint16_t gps_fail;
    uint16_t sd_err;
    uint32_t awake_sec;
    int16_t vmin_centi; // -1 when no pack voltage has been seen
    int16_t vmax_centi;
};

// One wake's modem steps. -1 means that step did not run.
// reason is a short token with no comma: at_fail, sim_timeout, skip, cmqtt_34, ...
struct NetStepTrace {
    int at;
    int sim;
    int reg;
    int netopen;
    int ip;
    char reason[24];
};

struct WakeLogFields {
    int level;
    uint32_t unix_time;
    const char* sim;
    const char* iccid;
    const char* plmn;
    int carrier_index;
    int rssi;
    int vmin_centi;
    int vmax_centi;
    int sd_present;
    int queue_depth;
    int net_fail;
    int gps_fail;
    int sd_err;
    uint32_t awake_sec;
    int net_at;
    int net_sim;
    int net_reg;
    int net_open;
    int net_ip;
    const char* net_err;
};

inline int daily_ci_equal(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

// Unknown text keeps fallback. "0".."3" match the enum.
inline int parse_log_level(const char* text, int fallback) {
    if (!text || !text[0]) return fallback;
    while (*text == ' ' || *text == '\t') text++;
    if (daily_ci_equal(text, "ERROR") || daily_ci_equal(text, "0")) return LOG_ERROR;
    if (daily_ci_equal(text, "WARN") || daily_ci_equal(text, "WARNING") || daily_ci_equal(text, "1")) return LOG_WARN;
    if (daily_ci_equal(text, "INFO") || daily_ci_equal(text, "2")) return LOG_INFO;
    if (daily_ci_equal(text, "DEBUG") || daily_ci_equal(text, "3")) return LOG_DEBUG;
    return fallback;
}

inline bool wake_log_enabled(int level, bool error, bool warn) {
    if (level <= LOG_ERROR) return error;
    if (level == LOG_WARN) return error || warn;
    return true;
}

inline int32_t local_day_number(uint32_t unix_utc, int32_t tz_offset_sec) {
    int64_t local = (int64_t)unix_utc + (int64_t)tz_offset_sec;
    if (local < 0) return -1;
    return (int32_t)(local / 86400LL);
}

// Howard Hinnant civil_from_days / days_from_civil. Day 0 is 1970-01-01.
inline void ymd_from_day(int32_t day, int* year, int* month, int* mday) {
    int64_t z = (int64_t)day + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    if (year) *year = (int)y;
    if (month) *month = (int)m;
    if (mday) *mday = (int)d;
}

inline int32_t day_from_ymd(int year, int month, int day) {
    int y = year - (month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)(era * 146097 + (int)doe - 719468);
}

inline void format_log_day(int32_t day, char out[9]) {
    int year = 1970;
    int month = 1;
    int mday = 1;
    ymd_from_day(day, &year, &month, &mday);
    if (year < 0) year = 0;
    if (year > 9999) year = 9999;
    snprintf(out, 9, "%04d%02d%02d", year, month, mday);
}

inline bool log_name_to_day(const char* name, int32_t* day_out) {
    if (!name || !day_out) return false;
    const char* base = name;
    for (const char* p = name; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    if (strlen(base) != 12) return false;
    for (int i = 0; i < 8; i++) {
        if (base[i] < '0' || base[i] > '9') return false;
    }
    if (base[8] != '.' || base[9] != 'l' || base[10] != 'o' || base[11] != 'g') return false;
    int year = (base[0] - '0') * 1000 + (base[1] - '0') * 100 + (base[2] - '0') * 10 + (base[3] - '0');
    int month = (base[4] - '0') * 10 + (base[5] - '0');
    int mday = (base[6] - '0') * 10 + (base[7] - '0');
    if (month < 1 || month > 12 || mday < 1 || mday > 31) return false;
    *day_out = day_from_ymd(year, month, mday);
    return true;
}

// Keep today and the previous 6 days. Age 7 and older is removed.
inline bool log_file_expired(int32_t file_day, int32_t today) {
    if (file_day < 0 || today < 0) return false;
    return (today - file_day) >= 7;
}

inline void health_clear_counters(DailyHealth* health, int32_t today) {
    if (!health) return;
    int32_t sent = health->sent_day;
    memset(health, 0, sizeof(*health));
    health->day = today;
    health->sent_day = sent;
    health->vmin_centi = -1;
    health->vmax_centi = -1;
}

inline void health_ensure_day(DailyHealth* health, int32_t today) {
    if (!health || today < 0) return;
    if (health->day < 0) {
        health->day = today;
        return;
    }
    if (health->day != today) health_clear_counters(health, today);
}

inline bool health_due(const DailyHealth* health, int32_t today, bool power_normal) {
    if (!health || !power_normal || today < 0) return false;
    return health->sent_day != today;
}

// reason values match esp_reset_reason_t. Deep sleep (8) is a normal wake.
inline void health_count_reset(DailyHealth* health, int reason) {
    if (!health) return;
    switch (reason) {
        case 8:
            return;
        case 5:
        case 6:
        case 7:
            if (health->rst_wdt < 65535) health->rst_wdt++;
            if (health->wdt < 65535) health->wdt++;
            break;
        case 4:
            if (health->rst_panic < 65535) health->rst_panic++;
            break;
        case 9:
            if (health->rst_brown < 65535) health->rst_brown++;
            break;
        case 3:
            if (health->rst_sw < 65535) health->rst_sw++;
            break;
        default:
            if (health->rst_other < 65535) health->rst_other++;
            break;
    }
}

inline void health_note_voltage_centi(DailyHealth* health, int centi) {
    if (!health || centi < 0) return;
    if (health->vmin_centi < 0 || centi < health->vmin_centi) health->vmin_centi = (int16_t)centi;
    if (health->vmax_centi < 0 || centi > health->vmax_centi) health->vmax_centi = (int16_t)centi;
}

inline void health_add_awake(DailyHealth* health, uint32_t sec) {
    if (!health || sec == 0) return;
    uint32_t sum = health->awake_sec + sec;
    if (sum < health->awake_sec) sum = 0xFFFFFFFFu;
    health->awake_sec = sum;
}

inline char log_level_letter(int level) {
    if (level <= LOG_ERROR) return 'E';
    if (level == LOG_WARN) return 'W';
    if (level >= LOG_DEBUG) return 'D';
    return 'I';
}

// Join the deep-sleep bring-up branch and the later failure. A forced
// power-off that then hits the SIM deadline is "pwr_off+deadline".
inline void compose_net_err(char* out, size_t out_len, const char* branch, const char* reason) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    int has_branch = branch && branch[0];
    int has_reason = reason && reason[0];
    if (has_branch && has_reason) {
        snprintf(out, out_len, "%s+%s", branch, reason);
        return;
    }
    if (has_branch) snprintf(out, out_len, "%s", branch);
    else if (has_reason) snprintf(out, out_len, "%s", reason);
}

// level,unix,sim,iccid,plmn,idx,rssi,vmin_cv,vmax_cv,sd,queue,net_fail,gps_fail,sd_err,awake,at,simrdy,reg,netopen,ip,err
inline int format_wake_log(const WakeLogFields* in, char* out, size_t out_len) {
    if (!in || !out || out_len < 8) return -1;
    const char* sim = (in->sim && in->sim[0]) ? in->sim : "-";
    const char* iccid = (in->iccid && in->iccid[0]) ? in->iccid : "-";
    const char* plmn = (in->plmn && in->plmn[0]) ? in->plmn : "-";
    char err_buf[24];
    const char* err_src = (in->net_err && in->net_err[0]) ? in->net_err : "-";
    size_t ei = 0;
    for (; err_src[ei] && ei + 1 < sizeof(err_buf); ei++) {
        char c = err_src[ei];
        if (c == ',' || c == '\n' || c == '\r' || c == ' ') c = '_';
        err_buf[ei] = c;
    }
    err_buf[ei] = '\0';
    int n = snprintf(out, out_len, "%c,%lu,%s,%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%lu,%d,%d,%d,%d,%d,%s",
        log_level_letter(in->level),
        (unsigned long)in->unix_time,
        sim, iccid, plmn,
        in->carrier_index, in->rssi, in->vmin_centi, in->vmax_centi, in->sd_present,
        in->queue_depth, in->net_fail, in->gps_fail, in->sd_err,
        (unsigned long)in->awake_sec,
        in->net_at, in->net_sim, in->net_reg, in->net_open, in->net_ip, err_buf);
    if (n < 0 || (size_t)n >= out_len) return -1;
    return n;
}

void daily_log_boot(int reset_reason);
void daily_log_note_net_fail();
void daily_log_note_gps_fail();
void daily_log_note_sd_error();
void daily_log_note_voltage(float volts);
void daily_log_set_level_text(const char* text);
void daily_log_set_level_code(int level);
int daily_log_level();
void daily_log_net_at(int ok);
void daily_log_net_sim(int ready);
void daily_log_net_reg(int status);
void daily_log_net_open(int ok);
void daily_log_net_ip(int has_ip);
void daily_log_net_reason(const char* reason);
void daily_log_net_branch(const char* branch);
void daily_log_net_clear_reason();
const NetStepTrace* daily_log_net_trace();
void write_daily_wake_log();
