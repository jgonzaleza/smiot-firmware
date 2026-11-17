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

static void test_baud_choice() {
    expect(sensor_cmd_select_baud(false, 0, true, 9600) == 9600, "the stored baud is used");
    expect(sensor_cmd_select_baud(true, 19200, true, 9600) == 19200, "BAUD overrides the stored rate");
    expect(sensor_cmd_select_baud(false, 0, false, 0) == 0, "no stored baud and no BAUD is no_baud");
    expect(sensor_cmd_select_baud(true, 100, true, 9600) == 0, "a BAUD outside 1200..115200 is no_baud");
    expect(sensor_cmd_select_baud(true, 115200, false, 0) == 115200, "BAUD alone is enough");
    expect(sensor_cmd_select_baud(false, 0, true, 300) == 0, "a stored rate outside the range is no_baud");
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
    test_baud_choice();
    test_sd_line();
    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
