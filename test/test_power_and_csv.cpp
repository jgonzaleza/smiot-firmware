#include "csv_parse.h"
#include "daily_log.h"
#include "firmware_update_request.h"
#include "gps_policy.h"
#include "network_policy.h"
#include "ota_verify.h"
#include "param_flags.h"
#include "power_policy.h"
#include "queue_pool.h"
#include "sketch_config.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;

static void expect(bool condition, const char* message) {
    if (!condition) {
        printf("FAIL %s\n", message);
        failures++;
    }
}

static void test_csv_keeps_last_token() {
    char out[CSV_MAX_TOKENS][CSV_TOKEN_MAX + 1];
    bool truncated = true;
    int count = csv_parse_tokens("voltage,current,temp", out, CSV_MAX_TOKENS, &truncated);
    expect(count == 3, "three tokens");
    expect(!truncated, "not truncated");
    expect(strcmp(out[2], "temp") == 0, "last token kept");
}

static void test_csv_bounds() {
    char out[CSV_MAX_TOKENS][CSV_TOKEN_MAX + 1];
    bool truncated = false;
    int count = csv_parse_tokens("a,b,c,d,e,f,g,h,i,j,k", out, CSV_MAX_TOKENS, &truncated);
    expect(count == CSV_MAX_TOKENS, "capped at 10");
    expect(truncated, "extra token flagged");
}

static void test_critical_blocks_radio() {
    PowerInput in = {};
    in.pack_valid = true;
    in.pack_volts = 9.5f;
    in.previous = POWER_NORMAL;
    in.backend_sleep_sec = 510;
    in.backend_reading_count = 2;
    PowerDecision decision = decide_power(in);
    expect(decision.mode == POWER_CRITICAL, "critical mode");
    expect(!decision.radio_allowed, "radio off");
    expect(decision.eff_sleep_sec >= POWER_CRITICAL_MIN_SLEEP_SEC, "critical sleep floor");
}

static void test_low_has_hysteresis() {
    PowerInput enter = {};
    enter.pack_valid = true;
    enter.pack_volts = 10.7f;
    enter.previous = POWER_NORMAL;
    enter.backend_sleep_sec = 510;
    enter.backend_reading_count = 2;
    PowerDecision low = decide_power(enter);
    expect(low.mode == POWER_LOW, "enter low");
    expect(low.eff_sleep_sec >= POWER_LOW_MIN_SLEEP_SEC, "low sleep floor");

    PowerInput stay = enter;
    stay.pack_volts = 11.0f;
    stay.previous = POWER_LOW;
    expect(decide_power(stay).mode == POWER_LOW, "stay low below 11.4");

    PowerInput leave = stay;
    leave.pack_volts = 11.5f;
    expect(decide_power(leave).mode == POWER_NORMAL, "leave low at 11.4");
}

static void test_pack_volts_from_raw_count() {
    PackVolts pv = pack_volts_from_raw(1199.0f, 0.01f);
    expect(pv.valid, "1199 * 0.01 is a valid pack voltage");
    float delta = pv.volts - 11.99f;
    if (delta < 0.0f) delta = -delta;
    expect(delta < 0.001f, "1199 * 0.01 is 11.99 V");

    PowerInput in = {};
    in.pack_volts = pv.volts;
    in.pack_valid = pv.valid;
    in.previous = POWER_NORMAL;
    in.backend_sleep_sec = 510;
    in.backend_reading_count = 1;
    expect(decide_power(in).mode == POWER_NORMAL, "11.99 V stays POWER_NORMAL");

    PackVolts zero = pack_volts_from_raw(0.0f, 0.01f);
    expect(!zero.valid, "raw 0 is not a pack voltage");

    PackVolts unscaled = pack_volts_from_raw(1199.0f, 1.0f);
    expect(!unscaled.valid, "1199 * 1.0 is out of range");

    PackVolts negative = pack_volts_from_raw(-1199.0f, 0.01f);
    expect(!negative.valid, "negative raw stays invalid");
}

static void test_missing_pack_keeps_backend() {
    PowerInput in = {};
    in.pack_valid = false;
    in.backend_sleep_sec = 510;
    in.backend_reading_count = 2;
    in.previous = POWER_NORMAL;
    PowerDecision decision = decide_power(in);
    expect(decision.radio_allowed, "radio stays available");
    expect(decision.eff_sleep_sec == 510, "backend interval kept");
    expect(!decision.changed, "mode unchanged");
}

static void test_queue_pool_drops_oldest_and_rejects_oversize() {
    char pool[32];
    memset(pool, 0, sizeof(pool));
    int length = 0;
    expect(queue_pool_append(pool, &length, 32, "one\n", 4), "append one");
    expect(queue_pool_append(pool, &length, 32, "two\n", 4), "append two");
    expect(strcmp(pool, "one\ntwo\n") == 0, "pool text");
    length = queue_pool_drop_oldest(pool, length, '\n');
    expect(strcmp(pool, "two\n") == 0, "oldest dropped");
    expect(length == 4, "length after drop");
    char big[40];
    memset(big, 'x', sizeof(big));
    int big_len = 0;
    expect(!queue_pool_append(big, &big_len, 32, big, 32), "reject payload that fills the pool");
    char tight[8];
    memset(tight, 0, sizeof(tight));
    int tight_len = 0;
    expect(queue_pool_append(tight, &tight_len, 8, "abcd\n", 5), "fits with room for NUL");
    expect(!queue_pool_append(tight, &tight_len, 8, "zzzz", 4), "no room for the next record");
    expect(strcmp(tight, "abcd\n") == 0, "tight pool unchanged");
}

static void test_hologram_iccid_strips_prefix() {
    const char* table = "89464:hologram,89883:emnify";
    ApnChoice choice = choose_apn("+ICCID: 89464278206109010629", NULL, table, DEFAULT_NETWORK_APN);
    expect(strcmp(choice.iccid, "89464278206109010629") == 0, "iccid digits only");
    expect(choice.set_apn, "hologram sets an APN");
    expect(strcmp(choice.apn, "hologram") == 0, "hologram apn");
    expect(strcmp(choice.sim, "hologram") == 0, "hologram sim");
    expect(!choice.unknown_iccid, "known prefix is not an error");
    expect(strcmp(sim_log_name(choice.sim), "Hologram") == 0, "log name Hologram");

    ApnChoice emnify = choose_apn("+ICCID: 89883000000000000000", NULL, table, DEFAULT_NETWORK_APN);
    expect(strcmp(emnify.sim, "emnify") == 0, "emnify prefix");
    expect(strcmp(emnify.apn, "emnify") == 0, "emnify apn");
}

static void test_unknown_iccid_uses_default_apn() {
    const char* table = "89464:hologram,89883:emnify";
    ApnChoice choice = choose_apn("+ICCID: 89148000000000000000", NULL, table, DEFAULT_NETWORK_APN);
    expect(choice.unknown_iccid, "unknown prefix records an error");
    expect(choice.set_apn, "unknown prefix still sets the default APN");
    expect(strcmp(choice.apn, "hologram") == 0, "default apn is hologram");
    expect(strcmp(choice.sim, "unknown") == 0, "unknown sim label");
    expect(strcmp(choice.sim, "local") != 0, "unknown is not local");
}

static void test_backend_apn_overrides_prefix() {
    const char* table = "89464:hologram,89883:emnify";
    ApnChoice choice = choose_apn("+ICCID: 89464278206109010629", "internet", table, DEFAULT_NETWORK_APN);
    expect(choice.set_apn, "backend apn is applied");
    expect(strcmp(choice.apn, "internet") == 0, "backend apn overrides the prefix");
    expect(strcmp(choice.sim, "hologram") == 0, "sim stays the prefix carrier");
    expect(!choice.unknown_iccid, "backend apn suppresses the unknown error");
}

static void test_backend_local_sets_no_apn() {
    const char* table = "89464:hologram,89883:emnify";
    ApnChoice local = choose_apn("+ICCID: 89464278206109010629", "local", table, DEFAULT_NETWORK_APN);
    expect(!local.set_apn, "local sets no APN");
    expect(strcmp(local.sim, "local") == 0, "local sim");
    expect(!local.unknown_iccid, "local is not an unknown-sim error");

    ApnChoice empty = choose_apn("+ICCID: 89464278206109010629", "", table, DEFAULT_NETWORK_APN);
    expect(!empty.set_apn, "empty backend APN sets no APN");
    expect(strcmp(empty.sim, "local") == 0, "empty backend APN is local");

    ApnChoice mixed = choose_apn("  +ICCID: 89464278206109010629  ", " LOCAL ", table, DEFAULT_NETWORK_APN);
    expect(!mixed.set_apn, "LOCAL is case insensitive");
    expect(strcmp(mixed.sim, "local") == 0, "trimmed LOCAL");
}

static RotationState test_rotation(int index, int failures, bool local, uint32_t sticky) {
    RotationState state;
    memset(&state, 0, sizeof(state));
    state.index = index;
    state.failures = failures;
    state.max_failures = 3;
    state.slot_count = 4;
    state.local_sim = local;
    state.sticky_since = sticky;
    return state;
}

static void test_rotation_advances_after_three_failures() {
    RotationState state = test_rotation(0, 3, false, 0);
    rotation_prepare(&state, 1, 86400);
    expect(state.index == 1, "three failures advance to the first PLMN");
    expect(state.failures == 0, "failure count resets after a rotation");
}

static void test_rotation_resets_on_success() {
    RotationState state = test_rotation(2, 2, false, 0);
    rotation_on_success(&state, 5000);
    expect(state.failures == 0, "success clears consecutive failures");
    expect(state.index == 2, "success keeps the current carrier");
    expect(state.sticky_since == 5000, "manual success starts the sticky timer");

    RotationState automatic = test_rotation(0, 2, false, 0);
    rotation_on_success(&automatic, 5000);
    expect(automatic.failures == 0, "automatic success clears failures");
    expect(automatic.sticky_since == 0, "automatic success does not start sticky");
}

static void test_rotation_wraps_to_automatic() {
    RotationState state = test_rotation(3, 3, false, 0);
    rotation_prepare(&state, 1, 86400);
    expect(state.index == 0, "last PLMN wraps back to automatic");
    expect(state.failures == 0, "wrap resets the failure count");
}

static void test_rotation_skips_local_sim() {
    RotationState state = test_rotation(2, 3, true, 100);
    rotation_on_failure(&state);
    expect(state.failures == 3, "local SIM does not count another failure");
    rotation_prepare(&state, 1, 86400);
    expect(state.index == 0, "local SIM is forced to automatic");
    expect(state.sticky_since == 0, "local SIM clears the sticky carrier");
    expect(state.failures == 0, "local SIM does not keep a rotation debt");
}

static void test_rotation_sticky_expiry_returns_to_automatic() {
    const uint32_t sticky_sec = (uint32_t)NET_STICKY_HOURS * 3600u;
    RotationState stay = test_rotation(2, 0, false, 1000);
    rotation_prepare(&stay, 1000 + sticky_sec - 1, sticky_sec);
    expect(stay.index == 2, "sticky carrier is kept until the window ends");

    RotationState expired = test_rotation(2, 0, false, 1000);
    rotation_prepare(&expired, 1000 + sticky_sec, sticky_sec);
    expect(expired.index == 0, "sticky expiry returns to automatic");
    expect(expired.sticky_since == 0, "sticky timestamp is cleared");
    expect(NET_STICKY_HOURS == 24, "default sticky window is 24 hours");
}

static void test_cops_numeric_plmn() {
    char plmn[8];
    expect(cops_numeric_plmn("\r\n+COPS: 0,2,\"71204\",7\r\nOK\r\n", plmn, sizeof(plmn)), "cops parsed");
    expect(strcmp(plmn, "71204") == 0, "plmn 71204");
    expect(!cops_numeric_plmn("+COPS: 0,0,\"Kolbi\",7", plmn, sizeof(plmn)), "name is not a plmn");
    expect(strcmp(SIM_PREFIXES, "89464:hologram,89883:emnify") == 0, "build default prefix table");
}

static DailyHealth test_health() {
    DailyHealth health;
    memset(&health, 0, sizeof(health));
    health.day = -1;
    health.sent_day = -1;
    health.vmin_centi = -1;
    health.vmax_centi = -1;
    return health;
}

static void test_daily_log_and_health() {
    expect(parse_log_level("INFO", LOG_ERROR) == LOG_INFO, "info level");
    expect(parse_log_level("error", LOG_INFO) == LOG_ERROR, "error level");
    expect(parse_log_level("nope", LOG_INFO) == LOG_INFO, "unknown level keeps the default");
    expect(wake_log_enabled(LOG_INFO, false, false), "info writes every wake");
    expect(!wake_log_enabled(LOG_ERROR, false, false), "error stays quiet without an error");
    expect(wake_log_enabled(LOG_ERROR, true, false), "error writes when something failed");
    expect(wake_log_enabled(LOG_WARN, false, true), "warn writes on a warning");

    expect(local_day_number(0, 0) == 0, "epoch is day 0");
    char ymd[9];
    format_log_day(0, ymd);
    expect(strcmp(ymd, "19700101") == 0, "day 0 is 1970-01-01");
    expect(day_from_ymd(1970, 1, 1) == 0, "ymd converts back");
    format_log_day(20000, ymd);
    int32_t round = -1;
    char named[16];
    snprintf(named, sizeof(named), "%s.log", ymd);
    expect(log_name_to_day(named, &round), "log name parses");
    expect(round == 20000, "log name round trip");
    expect(!log_file_expired(10, 16), "six days ago is kept");
    expect(log_file_expired(10, 17), "seven days ago is removed");

    DailyHealth health = test_health();
    health_count_reset(&health, 8);
    expect(health.wdt == 0 && health.rst_other == 0, "deep sleep is not a reset");
    health_count_reset(&health, 6);
    expect(health.wdt == 1 && health.rst_wdt == 1, "task watchdog is counted");
    health_count_reset(&health, 4);
    expect(health.rst_panic == 1, "panic is counted by cause");
    health.net_fail = 2;
    health_ensure_day(&health, 5);
    expect(health.day == 5 && health.net_fail == 2, "first dated day keeps early counters");
    health_ensure_day(&health, 6);
    expect(health.day == 6 && health.net_fail == 0, "a new day clears counters");
    expect(!health_due(&health, 6, false), "low or critical power skips the summary");
    expect(health_due(&health, 6, true), "first normal transmit of the day is due");
    health.sent_day = 6;
    expect(!health_due(&health, 6, true), "a later transmit the same day is not due");

    WakeLogFields fields;
    memset(&fields, 0, sizeof(fields));
    fields.level = LOG_INFO;
    fields.unix_time = 1710000000UL;
    fields.sim = "hologram";
    fields.iccid = "89464278206109010629";
    fields.plmn = "71204";
    fields.carrier_index = 0;
    fields.rssi = 18;
    fields.vmin_centi = 1190;
    fields.vmax_centi = 1240;
    fields.sd_present = 1;
    fields.queue_depth = 2;
    fields.awake_sec = 40;
    char line[192];
    expect(format_wake_log(&fields, line, sizeof(line)) > 0, "wake line fits");
    expect(line[0] == 'I', "info line starts with I");
    expect(strstr(line, "89464278206109010629") != NULL, "wake line has the cleaned iccid");
    expect(strstr(line, "71204") != NULL, "wake line has the plmn");
    expect(strchr(line, '\n') == NULL, "wake line is one record");
}

static void test_rain_command_rising_edge() {
    const bool one = flag_is_on(true, false, false, true, 1);
    const bool json_true = flag_is_on(true, true, true, false, 0);
    const bool zero = flag_is_on(true, false, false, true, 0);
    const bool json_false = flag_is_on(true, true, false, false, 0);

    expect(one, "number 1 is a command");
    expect(json_true, "JSON true is a command");
    expect(rain_pulse_due(one, false), "1 pulses when previous is off");
    expect(rain_pulse_due(json_true, false), "true pulses when previous is off");

    expect(!zero, "number 0 is not a command");
    expect(!json_false, "JSON false is not a command");
    expect(!rain_pulse_due(zero, false), "0 does not pulse");
    expect(!rain_pulse_due(json_false, false), "false does not pulse");

    bool previous_on = false;
    expect(rain_pulse_due(one, previous_on), "first 1 pulses");
    previous_on = true;
    expect(!rain_pulse_due(one, previous_on), "repeated 1 does not pulse");
    previous_on = zero;
    expect(rain_pulse_due(one, previous_on), "1 after a 0 pulses again");
}

static GpsSampleInput gps_sample_input(bool first_boot, bool modem_ready, bool get_gps_data, bool cache_ok, bool update_due) {
    GpsSampleInput in = {};
    in.first_boot = first_boot;
    in.modem_ready = modem_ready;
    in.get_gps_data = get_gps_data;
    in.cache_ok = cache_ok;
    in.update_due = update_due;
    return in;
}

static void test_gps_time_budget() {
    expect(GPS_COLD_BUDGET_SEC == 90, "cold budget default is 90 s");
    expect(GPS_WARM_BUDGET_SEC == 30, "warm budget default is 30 s");
    expect(GPS_FAIL_BUDGET_SEC == 15, "short budget default is 15 s");
    expect(GPS_POLL_MS == 2000, "gps poll default is 2000 ms");
    expect(gps_budget_sec(true, true, 0) == 90, "first boot uses 90 s");
    expect(gps_budget_sec(true, false, 0) == 90, "first boot without a cache uses 90 s");
    expect(gps_budget_sec(false, true, 0) == 30, "warm start with a cache uses 30 s");
    expect(gps_budget_sec(false, true, 2) == 30, "two misses still use the warm budget");
    expect(gps_budget_sec(false, false, 0) == 90, "no cache uses 90 s");
    expect(gps_budget_sec(false, true, 3) == 15, "three misses use 15 s");
    expect(gps_budget_sec(true, false, 4) == 15, "a failure streak uses 15 s on first boot");
    expect(gps_budget_sec(false, false, 255) == 15, "a full streak stays on 15 s");
    // A fix in get_gps_position sets gps_fail_streak back to 0. A timeout
    // increments it and stops at 255. That write stays next to the modem loop.
}

static void test_first_boot_takes_a_gps_fix() {
    expect(decide_gps_sample(gps_sample_input(true, true, false, true, false)) == GPS_TRY,
           "first boot with the modem on tries a fix");
    expect(decide_gps_sample(gps_sample_input(false, true, false, true, false)) == GPS_CACHE,
           "later wake with gps off and a valid cache stays on cache");
    expect(decide_gps_sample(gps_sample_input(false, true, false, true, true)) == GPS_TRY,
           "a due gps interval still tries a fix");
    expect(decide_gps_sample(gps_sample_input(false, true, true, true, false)) == GPS_TRY,
           "gps requested still tries a fix");
    expect(decide_gps_sample(gps_sample_input(true, false, false, true, false)) == GPS_CACHE,
           "first boot without the modem uses the cache");
    expect(decide_gps_sample(gps_sample_input(false, false, false, false, false)) == GPS_SKIP,
           "no modem and no cache is skipped");
}

struct UpdateCheckFields {
    bool has_client_id;
    bool has_device_type;
    bool has_variant;
    bool has_firmware_version;
    bool has_device_id;
    char client_id[80];
    char variant[32];
    char device_id[32];
    long device_type;
    long firmware_version;
};

struct JsonScan {
    const char* p;
    bool failed;
};

static void json_skip(JsonScan* j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') {
        j->p++;
    }
}

static bool json_string(JsonScan* j, char* out, size_t out_len) {
    if (*j->p != '"') {
        j->failed = true;
        return false;
    }
    j->p++;
    size_t n = 0;
    while (*j->p && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') {
            char escaped = *j->p;
            if (!escaped) {
                j->failed = true;
                return false;
            }
            j->p++;
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                c = (unsigned char)escaped;
                break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                for (int i = 0; i < 4; i++) {
                    char hex = *j->p++;
                    bool digit = (hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') || (hex >= 'A' && hex <= 'F');
                    if (!digit) {
                        j->failed = true;
                        return false;
                    }
                }
                c = '?';
                break;
            default:
                j->failed = true;
                return false;
            }
        } else if (c < 0x20) {
            j->failed = true;
            return false;
        }
        if (out) {
            if (n + 1 >= out_len) {
                j->failed = true;
                return false;
            }
            out[n++] = (char)c;
        }
    }
    if (*j->p != '"') {
        j->failed = true;
        return false;
    }
    j->p++;
    if (out) {
        out[n] = '\0';
    }
    return true;
}

static bool json_number(JsonScan* j, long* value, bool* is_int) {
    const char* start = j->p;
    if (*j->p == '-') {
        j->p++;
    }
    if (*j->p == '0') {
        j->p++;
    } else if (*j->p >= '1' && *j->p <= '9') {
        while (*j->p >= '0' && *j->p <= '9') {
            j->p++;
        }
    } else {
        j->failed = true;
        return false;
    }
    bool integral = true;
    if (*j->p == '.') {
        integral = false;
        j->p++;
        if (*j->p < '0' || *j->p > '9') {
            j->failed = true;
            return false;
        }
        while (*j->p >= '0' && *j->p <= '9') {
            j->p++;
        }
    }
    if (*j->p == 'e' || *j->p == 'E') {
        integral = false;
        j->p++;
        if (*j->p == '+' || *j->p == '-') {
            j->p++;
        }
        if (*j->p < '0' || *j->p > '9') {
            j->failed = true;
            return false;
        }
        while (*j->p >= '0' && *j->p <= '9') {
            j->p++;
        }
    }
    if (is_int) {
        *is_int = integral;
    }
    if (value && integral) {
        long parsed = 0;
        bool negative = *start == '-';
        const char* digit = negative ? start + 1 : start;
        while (digit < j->p) {
            parsed = parsed * 10 + (*digit - '0');
            digit++;
        }
        *value = negative ? -parsed : parsed;
    }
    return true;
}

static bool json_literal(JsonScan* j, const char* literal) {
    size_t length = strlen(literal);
    if (strncmp(j->p, literal, length) != 0) {
        j->failed = true;
        return false;
    }
    j->p += length;
    return true;
}

static bool json_skip_value(JsonScan* j);

static bool json_object(JsonScan* j, UpdateCheckFields* fields, bool capture) {
    if (*j->p != '{') {
        j->failed = true;
        return false;
    }
    j->p++;
    json_skip(j);
    if (*j->p == '}') {
        j->p++;
        return true;
    }
    for (;;) {
        char key[40];
        json_skip(j);
        if (!json_string(j, key, sizeof(key))) {
            return false;
        }
        json_skip(j);
        if (*j->p != ':') {
            j->failed = true;
            return false;
        }
        j->p++;
        json_skip(j);
        bool taken = false;
        if (capture && fields) {
            if (strcmp(key, "client_id") == 0 && *j->p == '"') {
                if (fields->has_client_id || !json_string(j, fields->client_id, sizeof(fields->client_id))) {
                    j->failed = true;
                    return false;
                }
                fields->has_client_id = true;
                taken = true;
            } else if (strcmp(key, "variant") == 0 && *j->p == '"') {
                if (fields->has_variant || !json_string(j, fields->variant, sizeof(fields->variant))) {
                    j->failed = true;
                    return false;
                }
                fields->has_variant = true;
                taken = true;
            } else if (strcmp(key, "device_id") == 0 && *j->p == '"') {
                if (fields->has_device_id || !json_string(j, fields->device_id, sizeof(fields->device_id))) {
                    j->failed = true;
                    return false;
                }
                fields->has_device_id = true;
                taken = true;
            } else if (strcmp(key, "device_type") == 0 && (*j->p == '-' || (*j->p >= '0' && *j->p <= '9'))) {
                bool integral = false;
                long value = 0;
                if (fields->has_device_type || !json_number(j, &value, &integral)) {
                    j->failed = true;
                    return false;
                }
                if (integral) {
                    fields->device_type = value;
                    fields->has_device_type = true;
                }
                taken = true;
            } else if (strcmp(key, "firmware_version") == 0 && (*j->p == '-' || (*j->p >= '0' && *j->p <= '9'))) {
                bool integral = false;
                long value = 0;
                if (fields->has_firmware_version || !json_number(j, &value, &integral)) {
                    j->failed = true;
                    return false;
                }
                if (integral) {
                    fields->firmware_version = value;
                    fields->has_firmware_version = true;
                }
                taken = true;
            }
        }
        if (!taken && !json_skip_value(j)) {
            return false;
        }
        json_skip(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p == '}') {
            j->p++;
            return true;
        }
        j->failed = true;
        return false;
    }
}

static bool json_array(JsonScan* j) {
    if (*j->p != '[') {
        j->failed = true;
        return false;
    }
    j->p++;
    json_skip(j);
    if (*j->p == ']') {
        j->p++;
        return true;
    }
    for (;;) {
        if (!json_skip_value(j)) {
            return false;
        }
        json_skip(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        j->failed = true;
        return false;
    }
}

static bool json_skip_value(JsonScan* j) {
    json_skip(j);
    if (*j->p == '"') {
        return json_string(j, NULL, 0);
    }
    if (*j->p == '-' || (*j->p >= '0' && *j->p <= '9')) {
        return json_number(j, NULL, NULL);
    }
    if (*j->p == '{') {
        return json_object(j, NULL, false);
    }
    if (*j->p == '[') {
        return json_array(j);
    }
    if (*j->p == 't') {
        return json_literal(j, "true");
    }
    if (*j->p == 'f') {
        return json_literal(j, "false");
    }
    if (*j->p == 'n') {
        return json_literal(j, "null");
    }
    j->failed = true;
    return false;
}

static bool parse_update_check_body(const char* json, UpdateCheckFields* fields) {
    memset(fields, 0, sizeof(*fields));
    if (!json) {
        return false;
    }
    JsonScan scan = {json, false};
    json_skip(&scan);
    if (!json_object(&scan, fields, true) || scan.failed) {
        return false;
    }
    json_skip(&scan);
    return *scan.p == '\0' && !scan.failed;
}

static void test_firmware_update_body_is_json() {
    const char* client_id = "A7608-S3-01-005";
    const uint64_t device_id = 0xA1B2C3D4E5F6ULL;
    char device_text[32];
    snprintf(device_text, sizeof(device_text), "%llu", (unsigned long long)device_id);

    char body[256];
    int length = firmware_update_request_body(body, sizeof(body), client_id, 2, "cell", 400, device_id);
    expect(length > 0, "cell update body fits");
    UpdateCheckFields fields;
    expect(parse_update_check_body(body, &fields), "cell update body parses");
    expect(fields.has_client_id && strcmp(fields.client_id, client_id) == 0, "client_id");
    expect(fields.has_device_type && fields.device_type == 2, "device_type");
    expect(fields.has_variant && strcmp(fields.variant, "cell") == 0, "variant");
    expect(fields.has_firmware_version && fields.firmware_version == 400, "firmware_version");
    expect(fields.has_device_id && strcmp(fields.device_id, device_text) == 0, "device_id");

    length = firmware_update_request_body(body, sizeof(body), client_id, 3, "wifi", 400, device_id);
    expect(length > 0, "wifi update body fits");
    expect(parse_update_check_body(body, &fields), "wifi update body parses");
    expect(fields.has_variant && strcmp(fields.variant, "wifi") == 0, "wifi variant");
    expect(fields.has_device_type && fields.device_type == 3, "wifi device_type");

    const char* broken =
        "{\"client_id\": \"A7608-S3-01-005\", \"device_type\":2, \"variant\":\"cell, \"firmware_version\":400, \"device_id\": \"1\"}";
    expect(!parse_update_check_body(broken, &fields), "missing variant quote is not json");

    expect(strcmp(CHECK_FIRMWARE_UPDATE_URL,
                  "https://junkzqt2yc.execute-api.us-east-1.amazonaws.com/pro/device_firmware_config") == 0,
           "default firmware update url");
}

static void test_missing_signature_rejects_ota() {
    const char* sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char* other = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    expect(ota_precheck(NULL, "AAAA", sha) == OTA_PRECHECK_REJECT_MISSING, "missing hash rejects");
    expect(ota_precheck("", "AAAA", sha) == OTA_PRECHECK_REJECT_MISSING, "empty hash rejects");
    expect(ota_precheck(sha, NULL, sha) == OTA_PRECHECK_REJECT_MISSING, "missing signature rejects");
    expect(ota_precheck(sha, "", sha) == OTA_PRECHECK_REJECT_MISSING, "empty signature rejects");
    expect(ota_precheck(sha, "AAAA", other) == OTA_PRECHECK_REJECT_HASH, "hash mismatch rejects");
    expect(ota_precheck("abcd", "AAAA", sha) == OTA_PRECHECK_REJECT_HASH, "short hash rejects");
    // A present signature is not acceptance. ECDSA stays on the device.
}

int main() {
    test_csv_keeps_last_token();
    test_csv_bounds();
    test_critical_blocks_radio();
    test_low_has_hysteresis();
    test_pack_volts_from_raw_count();
    test_missing_pack_keeps_backend();
    test_queue_pool_drops_oldest_and_rejects_oversize();
    test_hologram_iccid_strips_prefix();
    test_unknown_iccid_uses_default_apn();
    test_backend_apn_overrides_prefix();
    test_backend_local_sets_no_apn();
    test_rotation_advances_after_three_failures();
    test_rotation_resets_on_success();
    test_rotation_wraps_to_automatic();
    test_rotation_skips_local_sim();
    test_rotation_sticky_expiry_returns_to_automatic();
    test_cops_numeric_plmn();
    test_rain_command_rising_edge();
    test_first_boot_takes_a_gps_fix();
    test_gps_time_budget();
    test_missing_signature_rejects_ota();
    test_firmware_update_body_is_json();
    test_daily_log_and_health();
    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
