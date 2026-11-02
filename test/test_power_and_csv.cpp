#include "csv_parse.h"
#include "power_policy.h"

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

int main() {
    test_csv_keeps_last_token();
    test_csv_bounds();
    test_critical_blocks_radio();
    test_low_has_hysteresis();
    test_missing_pack_keeps_backend();
    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
