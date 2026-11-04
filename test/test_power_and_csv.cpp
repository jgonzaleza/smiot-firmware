#include "csv_parse.h"
#include "network_policy.h"
#include "power_policy.h"
#include "queue_pool.h"
#include "sketch_config.h"

#include <stdio.h>
#include <string.h>

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

static void test_cops_numeric_plmn() {
    char plmn[8];
    expect(cops_numeric_plmn("\r\n+COPS: 0,2,\"71204\",7\r\nOK\r\n", plmn, sizeof(plmn)), "cops parsed");
    expect(strcmp(plmn, "71204") == 0, "plmn 71204");
    expect(!cops_numeric_plmn("+COPS: 0,0,\"Kolbi\",7", plmn, sizeof(plmn)), "name is not a plmn");
    expect(strcmp(SIM_PREFIXES, "89464:hologram,89883:emnify") == 0, "build default prefix table");
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
    test_cops_numeric_plmn();
    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
