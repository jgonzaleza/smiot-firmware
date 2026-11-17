#include "sensor_cmd.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(bool condition, const char* message) {
    if (!condition) {
        printf("FAIL %s\n", message);
        failures++;
    }
}

static void test_function_06_parses() {
    SensorCmdFrame frame;
    sensor_cmd_parse("01 06 00 FF 00 08", &frame);
    expect(frame.send, "function 06 is sent");
    expect(frame.err == NULL, "function 06 has no error");
    expect(frame.addr == 0x01, "function 06 address");
    expect(frame.function == 0x06, "function 06 code");
    expect(frame.reg == 0x00FF, "function 06 register");
    expect(frame.quantity == 1, "function 06 quantity");
    expect(frame.values[0] == 0x0008, "function 06 value");
}

static void test_function_10_parses() {
    SensorCmdFrame frame;
    sensor_cmd_parse("01 10 00 FF 00 02 04 00 08 00 01", &frame);
    expect(frame.send, "function 10 is sent");
    expect(frame.err == NULL, "function 10 has no error");
    expect(frame.addr == 0x01, "function 10 address");
    expect(frame.function == 0x10, "function 10 code");
    expect(frame.reg == 0x00FF, "function 10 register");
    expect(frame.quantity == 2, "function 10 quantity");
    expect(frame.values[0] == 0x0008, "function 10 first value");
    expect(frame.values[1] == 0x0001, "function 10 second value");
}

static void test_rejected_frames_are_not_sent() {
    SensorCmdFrame bad_hex;
    sensor_cmd_parse("01 0G 00 FF 00 08", &bad_hex);
    expect(!bad_hex.send, "bad hex is not sent");
    expect(bad_hex.err && strcmp(bad_hex.err, "bad_frame") == 0, "bad hex is bad_frame");

    SensorCmdFrame split;
    sensor_cmd_parse("01 0 6 00 FF 00 08", &split);
    expect(!split.send, "a split nibble is not sent");

    SensorCmdFrame crc;
    sensor_cmd_parse("01 06 00 FF 00 08 B8 3C", &crc);
    expect(!crc.send, "a frame that already has a CRC is not sent");
    expect(crc.err && strcmp(crc.err, "bad_frame") == 0, "extra CRC bytes are bad_frame");

    SensorCmdFrame short_frame;
    sensor_cmd_parse("01 06 00 FF 00", &short_frame);
    expect(!short_frame.send, "a short function 06 is not sent");

    SensorCmdFrame wrong_count;
    sensor_cmd_parse("01 10 00 FF 00 02 03 00 08 00", &wrong_count);
    expect(!wrong_count.send, "a wrong byte count is not sent");
    expect(wrong_count.err && strcmp(wrong_count.err, "bad_frame") == 0, "wrong byte count is bad_frame");

    SensorCmdFrame mismatch;
    sensor_cmd_parse("01 10 00 FF 00 02 04 00 08", &mismatch);
    expect(!mismatch.send, "a short value list is not sent");

    SensorCmdFrame other;
    sensor_cmd_parse("01 03 00 FF 00 02", &other);
    expect(!other.send, "function 03 is not sent");
    expect(other.err && strcmp(other.err, "bad_function") == 0, "function 03 is bad_function");

    SensorCmdFrame empty;
    sensor_cmd_parse("", &empty);
    expect(!empty.send, "an empty frame is not sent");
    expect(empty.err && strcmp(empty.err, "bad_frame") == 0, "an empty frame is bad_frame");
}

static void test_modbus_result_names() {
    expect(strcmp(sensor_cmd_result_error(0x00), "success") == 0, "0x00 is success");
    expect(strcmp(sensor_cmd_result_error(0x01), "illegal_function") == 0, "0x01 is illegal_function");
    expect(strcmp(sensor_cmd_result_error(0x02), "illegal_data_address") == 0, "0x02 is illegal_data_address");
    expect(strcmp(sensor_cmd_result_error(0x03), "illegal_data_value") == 0, "0x03 is illegal_data_value");
    expect(strcmp(sensor_cmd_result_error(0x04), "slave_device_failure") == 0, "0x04 is slave_device_failure");
    expect(strcmp(sensor_cmd_result_error(0xE0), "invalid_slave_id") == 0, "0xE0 is invalid_slave_id");
    expect(strcmp(sensor_cmd_result_error(0xE1), "invalid_function") == 0, "0xE1 is invalid_function");
    expect(strcmp(sensor_cmd_result_error(0xE2), "no_response") == 0, "0xE2 is no_response");
    expect(strcmp(sensor_cmd_result_error(0xE3), "bad_crc") == 0, "0xE3 is bad_crc");
    expect(strcmp(sensor_cmd_result_error(0x11), "modbus_error") == 0, "an unknown code is modbus_error");
}

static void test_run_once_and_hash() {
    expect(sensor_cmd_should_run(true, 17, 0), "a new id runs");
    expect(!sensor_cmd_should_run(true, 17, 17), "the same id does not run");
    expect(!sensor_cmd_should_run(true, 0, 0), "id 0 does not run");
    expect(!sensor_cmd_should_run(false, 17, 0), "a missing id does not run");
    expect(!sensor_cmd_parameters_unchanged(true, false, false, true),
           "a matching parameter hash still runs an unfinished id");
    expect(sensor_cmd_parameters_unchanged(true, false, false, false),
           "a matching hash skips when the command is finished");
    expect(!sensor_cmd_parameters_unchanged(false, false, false, false),
           "a new parameter body is not skipped");
    expect(!sensor_cmd_parameters_unchanged(true, true, false, false),
           "a new restart is not skipped");
    expect(!sensor_cmd_parameters_unchanged(true, false, true, false),
           "a rain edge is not skipped");
}

static void test_restart_decision() {
    expect(sensor_cmd_restarts_on(true), "success requests a restart");
    expect(!sensor_cmd_restarts_on(false), "failure does not request a restart");
}

static void test_restart_rising_edge() {
    uint32_t next = 99;
    expect(restart_rising_edge(0, true, 1, &next), "0 then 1 restarts");
    expect(next == 1, "0 then 1 stores 1");

    expect(!restart_rising_edge(1, true, 1, &next), "1 then 1 does not restart");
    expect(next == 1, "1 then 1 keeps 1");

    expect(!restart_rising_edge(1, true, 0, &next), "1 then 0 does not restart");
    expect(next == 0, "1 then 0 stores 0");
    expect(restart_rising_edge(next, true, 1, &next), "1 then 0 then 1 restarts again");
    expect(next == 1, "1 then 0 then 1 stores 1");

    next = 0;
    expect(!restart_rising_edge(1, false, 1, &next), "a null RESTART does not restart");
    expect(next == 1, "a null RESTART keeps the last value");
    expect(!restart_rising_edge(0, false, 0, &next), "a null RESTART does not restart from 0");
    expect(next == 0, "a null RESTART keeps 0");
}

static void test_sensor_cmd_awake_guard() {
    expect(!sensor_cmd_skip_this_wake(0), "a fresh wake runs SENSOR_CMD");
    expect(!sensor_cmd_skip_this_wake(SENSOR_CMD_AWAKE_SKIP_MS), "240s still runs SENSOR_CMD");
    expect(sensor_cmd_skip_this_wake(SENSOR_CMD_AWAKE_SKIP_MS + 1), "more than 240s skips SENSOR_CMD");
}

static void test_report_attaches_once() {
    SensorCmdReport report;
    sensor_cmd_report_set(&report, 17, false, "no_response", "01 86 02");
    SensorCmdAttach first;
    expect(sensor_cmd_take_report(&report, &first), "the report attaches");
    expect(first.include, "the first attach is included");
    expect(first.id == 17, "sc_id");
    expect(first.ok == 0, "sc_ok is 0");
    expect(strcmp(first.err, "no_response") == 0, "sc_err");
    expect(strcmp(first.rsp, "01 86 02") == 0, "sc_rsp");

    SensorCmdAttach second;
    expect(!sensor_cmd_take_report(&report, &second), "the report is not attached again");
    expect(!second.include, "the second attach is empty");

    SensorCmdReport ok_report;
    sensor_cmd_report_set(&ok_report, 18, true, "success", NULL);
    SensorCmdAttach ok_attach;
    expect(sensor_cmd_take_report(&ok_report, &ok_attach), "a success report attaches");
    expect(ok_attach.id == 18 && ok_attach.ok == 1, "success is sc_ok 1");
    expect(ok_attach.err[0] == '\0' && ok_attach.rsp[0] == '\0', "success leaves sc_err and sc_rsp empty");
}

static bool copy_json_string(const char* json, const char* key, char* out, size_t out_len) {
    if (!json || !key || !out || out_len == 0) return false;
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* found = strstr(json, pattern);
    if (!found) return false;
    const char* colon = strchr(found + strlen(pattern), ':');
    if (!colon) return false;
    const char* quote = strchr(colon, '"');
    if (!quote) return false;
    quote++;
    size_t n = 0;
    while (quote[n] && quote[n] != '"' && n + 1 < out_len) {
        out[n] = quote[n];
        n++;
    }
    if (quote[n] != '"') return false;
    out[n] = '\0';
    return true;
}

static const char* k_parameters_topic = "smartiotcr/parameters/v2/224594735082620";

static const char* k_parameters_message =
    "{\n"
    "  \"TTS_SEC\": 300,\n"
    "  \"TTR_SENSOR\": 5000,\n"
    "  \"R_COUNT\": 1,\n"
    "  \"C_F_UPDATE\": 86400000,\n"
    "  \"L_LAT\": 10.01935,\n"
    "  \"L_LONG\": 84.09909,\n"
    "  \"G_GPS_DATA\": 0,\n"
    "  \"RR_SENSOR\": 0,\n"
    "  \"TB_READINGS\": 2,\n"
    "  \"R_RATE\": 2,\n"
    "  \"CAL\": 0,\n"
    "  \"RESTART\": 0,\n"
    "  \"SENSOR_CMD\": {\n"
    "    \"CMD_ID\": 1,\n"
    "    \"SENSOR_ID\": \"v_s_p\",\n"
    "    \"FRAME\": \"04 06 00 FD 00 02\"\n"
    "  }\n"
    "}\n";

static void test_parameters_message_fits_and_parses() {
    expect(strcmp(k_parameters_topic, "smartiotcr/parameters/v2/224594735082620") == 0, "the real topic");
    expect(strstr(k_parameters_message, "BAUD") == NULL, "the message has no BAUD");
    expect(cellular_mqtt_rx_fits(k_parameters_topic, k_parameters_message, CELLULAR_MQTT_RX_BYTES),
           "the topic plus the pretty-printed body fits 4096");
    expect(!cellular_mqtt_rx_fits(k_parameters_topic, k_parameters_message, 256),
           "the same body does not fit the old 256 buffer");

    char frame_text[64];
    char sensor_id[40];
    expect(copy_json_string(k_parameters_message, "FRAME", frame_text, sizeof(frame_text)), "FRAME is present");
    expect(copy_json_string(k_parameters_message, "SENSOR_ID", sensor_id, sizeof(sensor_id)), "SENSOR_ID is present");
    expect(strcmp(sensor_id, "v_s_p") == 0, "the bench id is v_s_p");
    expect(strcmp(frame_text, "04 06 00 FD 00 02") == 0, "FRAME is the bench write");

    SensorCmdFrame frame;
    sensor_cmd_parse(frame_text, &frame);
    expect(frame.send, "the bench frame is sent");
    expect(frame.err == NULL, "the bench frame has no error");
    expect(frame.addr == 4, "address 4");
    expect(frame.function == 6, "function 6");
    expect(frame.reg == 253, "register 253");
    expect(frame.values[0] == 2, "value 2");

    SensorCmdSlot slots[2];
    slots[0].id = "rain";
    slots[0].baud = 4800;
    slots[1].id = sensor_id;
    slots[1].baud = 9600;
    SensorCmdChoice match;
    sensor_cmd_choose(sensor_id, slots, 2, &match);
    expect(match.send && match.baud == 9600 && match.err == NULL,
           "a matching SENSOR_ID selects that sensor's stored baud");

    SensorCmdChoice suffix;
    sensor_cmd_choose("v_s_p_0", slots, 2, &suffix);
    expect(!suffix.send && suffix.err && strcmp(suffix.err, "unknown_sensor") == 0,
           "a telemetry suffix is not the stored id");

    SensorCmdChoice missing;
    sensor_cmd_choose("not-a-sensor", slots, 2, &missing);
    expect(!missing.send && missing.baud == 0, "a SENSOR_ID that matches nothing sends nothing");
    expect(missing.err && strcmp(missing.err, "unknown_sensor") == 0,
           "a SENSOR_ID that matches nothing reports unknown_sensor");
}

static void test_stored_baud_only() {
    SensorCmdSlot slots[1];
    slots[0].id = "v_s_p";
    slots[0].baud = 19200;
    SensorCmdChoice choice;
    sensor_cmd_choose("v_s_p", slots, 1, &choice);
    expect(choice.send && choice.baud == 19200, "the stored baud is the port speed");

    slots[0].baud = 300;
    sensor_cmd_choose("v_s_p", slots, 1, &choice);
    expect(!choice.send && choice.err && strcmp(choice.err, "no_baud") == 0,
           "a stored rate outside 1200..115200 sends nothing");

    sensor_cmd_choose("", slots, 1, &choice);
    expect(!choice.send && strcmp(choice.err, "unknown_sensor") == 0, "an empty id matches nothing");
}

static void test_awake_skip_reports_late() {
    SensorCmdReport report;
    sensor_cmd_report_set(&report, 1, false, "late", NULL);
    expect(report.pending && report.ok == 0, "awake skip is sc_ok 0");
    expect(strcmp(report.err, "late") == 0, "awake skip reports sc_err late");
    expect(report.id == 1, "awake skip keeps the command id");
}

static void test_sd_line() {
    char line[180];
    int n = sensor_cmd_format_sd_line(line, sizeof(line), 1700000000UL, 17, 1, "rain-1", "01 06 00 FF 00 08");
    expect(n > 0, "the sd line fits");
    expect(strcmp(line, "cmd,1700000000,17,1,rain-1,01 06 00 FF 00 08") == 0, "the sd line is cmd,...");
}

int main() {
    test_function_06_parses();
    test_function_10_parses();
    test_rejected_frames_are_not_sent();
    test_modbus_result_names();
    test_run_once_and_hash();
    test_restart_decision();
    test_restart_rising_edge();
    test_sensor_cmd_awake_guard();
    test_report_attaches_once();
    test_parameters_message_fits_and_parses();
    test_stored_baud_only();
    test_awake_skip_reports_late();
    test_sd_line();
    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
