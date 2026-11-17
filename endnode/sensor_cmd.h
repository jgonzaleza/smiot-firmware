#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// One-shot SENSOR_CMD from the parameters message. Plain functions so the
// host test can compile them. The station applies the result in sensor_cmd.cpp.
//
// The object is CMD_ID, SENSOR_ID, and FRAME. There is no BAUD field.
// SENSOR_ID must equal a SENSOR_ID stored in SENSOR_1..SENSOR_8. The RS485
// port uses that row's BAUD_RATE. FRAME is the address, the function and the
// data, with no CRC, and is sent as is to the address in its first byte.
// ModbusMaster adds the CRC. Function 06 and function 16 (0x10) are the only
// frames sent. No matching SENSOR_ID sends nothing.

static const int SENSOR_CMD_MAX_REGS = 64;
static const int SENSOR_CMD_BAUD_MIN = 1200;
static const int SENSOR_CMD_BAUD_MAX = 115200;

// Cellular MQTT stores the topic, a NUL, and the payload in one buffer.
static const size_t CELLULAR_MQTT_RX_BYTES = 4096;

inline bool cellular_mqtt_rx_fits(const char* topic, const char* payload, size_t buffer_bytes) {
    if (!topic || !payload || buffer_bytes == 0) return false;
    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);
    if (topic_len >= buffer_bytes) return false;
    return topic_len + 1 + payload_len <= buffer_bytes;
}

static const int SENSOR_CMD_MB_SUCCESS = 0x00;
static const int SENSOR_CMD_MB_ILLEGAL_FUNCTION = 0x01;
static const int SENSOR_CMD_MB_ILLEGAL_ADDRESS = 0x02;
static const int SENSOR_CMD_MB_ILLEGAL_VALUE = 0x03;
static const int SENSOR_CMD_MB_SLAVE_FAILURE = 0x04;
static const int SENSOR_CMD_MB_INVALID_SLAVE = 0xE0;
static const int SENSOR_CMD_MB_INVALID_FUNCTION = 0xE1;
static const int SENSOR_CMD_MB_NO_RESPONSE = 0xE2;
static const int SENSOR_CMD_MB_BAD_CRC = 0xE3;

struct SensorCmdFrame {
    bool send;
    const char* err;
    uint8_t addr;
    uint8_t function;
    uint16_t reg;
    uint16_t quantity;
    uint16_t values[SENSOR_CMD_MAX_REGS];
};

struct SensorCmdReport {
    uint32_t id;
    int ok;
    char err[32];
    char rsp[80];
    bool pending;
};

struct SensorCmdAttach {
    bool include;
    uint32_t id;
    int ok;
    char err[32];
    char rsp[80];
};

inline int sensor_cmd_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Whitespace between bytes is optional. A split nibble, odd length, or a
// non-hex character is rejected. count is set only on success.
inline bool sensor_cmd_parse_bytes(const char* text, uint8_t* out, int cap, int* count) {
    if (!text || !out || !count || cap <= 0) return false;
    int n = 0;
    int hi = -1;
    for (const char* p = text; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (hi >= 0) return false;
            continue;
        }
        int v = sensor_cmd_hex_nibble(c);
        if (v < 0) return false;
        if (hi < 0) {
            hi = v;
            continue;
        }
        if (n >= cap) return false;
        out[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    if (hi >= 0 || n == 0) return false;
    *count = n;
    return true;
}

inline void sensor_cmd_parse(const char* text, SensorCmdFrame* frame) {
    if (!frame) return;
    memset(frame, 0, sizeof(*frame));
    frame->err = "bad_frame";
    uint8_t raw[7 + SENSOR_CMD_MAX_REGS * 2];
    int count = 0;
    if (!sensor_cmd_parse_bytes(text, raw, (int)sizeof(raw), &count) || count < 2) return;

    uint8_t function = raw[1];
    if (function != 0x06 && function != 0x10) {
        frame->err = "bad_function";
        return;
    }
    if (function == 0x06) {
        if (count != 6) return;
        frame->send = true;
        frame->err = NULL;
        frame->addr = raw[0];
        frame->function = 0x06;
        frame->reg = (uint16_t)((raw[2] << 8) | raw[3]);
        frame->quantity = 1;
        frame->values[0] = (uint16_t)((raw[4] << 8) | raw[5]);
        return;
    }
    if (count < 7) return;
    uint16_t reg = (uint16_t)((raw[2] << 8) | raw[3]);
    uint16_t quantity = (uint16_t)((raw[4] << 8) | raw[5]);
    uint8_t nbytes = raw[6];
    if (quantity < 1 || quantity > SENSOR_CMD_MAX_REGS) return;
    if (nbytes != (uint8_t)(quantity * 2)) return;
    if (count != 7 + (int)nbytes) return;
    frame->send = true;
    frame->err = NULL;
    frame->addr = raw[0];
    frame->function = 0x10;
    frame->reg = reg;
    frame->quantity = quantity;
    for (uint16_t i = 0; i < quantity; i++) {
        int off = 7 + (int)i * 2;
        frame->values[i] = (uint16_t)((raw[off] << 8) | raw[off + 1]);
    }
}

// A new non-zero id runs once. The same id, a missing id, and 0 do not.
inline bool sensor_cmd_should_run(bool id_present, uint32_t cmd_id, uint32_t finished_id) {
    if (!id_present || cmd_id == 0) return false;
    if (cmd_id == finished_id) return false;
    return true;
}

// Retained parameters are skipped when the body, the restart edge and the rain
// edge are unchanged. An unfinished command still runs when the hash matches.
inline bool sensor_cmd_parameters_unchanged(bool hash_matches, bool restart_requested,
                                            bool pulse_rain, bool cmd_should_run) {
    if (cmd_should_run) return false;
    return hash_matches && !restart_requested && !pulse_rain;
}

// RESTART is a 0-to-1 edge. last is the stored rsprev value.
// present is false when the key is missing or null: next stays last, and
// this does not restart. A present value is the one to store. Only a stored
// 0 followed by 1 restarts.
inline bool restart_rising_edge(uint32_t last, bool present, uint32_t value, uint32_t* next) {
    if (!next) return false;
    if (!present) {
        *next = last;
        return false;
    }
    *next = value;
    return last == 0 && value == 1;
}

// More than 240s awake, the same span as AWAKE_BUDGET_MS. The command waits
// for a later wake, and CMD_ID stays unfinished.
static const uint32_t SENSOR_CMD_AWAKE_SKIP_MS = 240000UL;

inline bool sensor_cmd_skip_this_wake(uint32_t awake_ms) {
    return awake_ms > SENSOR_CMD_AWAKE_SKIP_MS;
}

inline bool sensor_cmd_restarts_on(bool write_ok) {
    return write_ok;
}

// ModbusMaster 2.0.1 result codes. 0x00 is success. 0x01-0x04 are the sensor
// exception. 0xE0-0xE3 are the library's own failures.
inline const char* sensor_cmd_result_error(int modbus_result) {
    switch (modbus_result) {
        case SENSOR_CMD_MB_SUCCESS: return "success";
        case SENSOR_CMD_MB_ILLEGAL_FUNCTION: return "illegal_function";
        case SENSOR_CMD_MB_ILLEGAL_ADDRESS: return "illegal_data_address";
        case SENSOR_CMD_MB_ILLEGAL_VALUE: return "illegal_data_value";
        case SENSOR_CMD_MB_SLAVE_FAILURE: return "slave_device_failure";
        case SENSOR_CMD_MB_INVALID_SLAVE: return "invalid_slave_id";
        case SENSOR_CMD_MB_INVALID_FUNCTION: return "invalid_function";
        case SENSOR_CMD_MB_NO_RESPONSE: return "no_response";
        case SENSOR_CMD_MB_BAD_CRC: return "bad_crc";
        default: return "modbus_error";
    }
}

inline bool sensor_cmd_baud_in_range(int baud) {
    return baud >= SENSOR_CMD_BAUD_MIN && baud <= SENSOR_CMD_BAUD_MAX;
}

// One stored sensor row. id is the SENSOR_ID in SENSOR_1..SENSOR_8, not a
// telemetry key. Readings append _0 and _1; those suffixes are not the id.
struct SensorCmdSlot {
    const char* id;
    int baud;
};

struct SensorCmdChoice {
    bool send;
    int baud;
    const char* err;
};

// A matching SENSOR_ID selects that row's stored baud. No match does not
// send and sets unknown_sensor. A match whose BAUD_RATE is outside
// 1200..115200 does not send and sets no_baud.
inline void sensor_cmd_choose(const char* sensor_id, const SensorCmdSlot* slots, int count,
                              SensorCmdChoice* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->err = "unknown_sensor";
    if (!sensor_id || !sensor_id[0] || !slots || count <= 0) return;
    for (int i = 0; i < count; i++) {
        const char* id = slots[i].id;
        if (!id || !id[0]) continue;
        if (strcmp(id, sensor_id) != 0) continue;
        if (!sensor_cmd_baud_in_range(slots[i].baud)) {
            out->err = "no_baud";
            return;
        }
        out->send = true;
        out->baud = slots[i].baud;
        out->err = NULL;
        return;
    }
}

inline void sensor_cmd_copy_token(char* dst, size_t dst_len, const char* src) {
    if (!dst || dst_len == 0) return;
    if (!src || !src[0]) {
        dst[0] = '-';
        if (dst_len > 1) dst[1] = '\0';
        return;
    }
    size_t n = 0;
    for (; src[n] && n + 1 < dst_len; n++) {
        char c = src[n];
        if (c == ',' || c == '\n' || c == '\r') c = '_';
        dst[n] = c;
    }
    dst[n] = '\0';
}

// cmd,<unix>,<id>,<ok>,<sensor>,<frame>
inline int sensor_cmd_format_sd_line(char* out, size_t out_len, uint32_t unix_time, uint32_t id,
                                     int ok, const char* sensor_id, const char* frame) {
    if (!out || out_len < 8) return -1;
    char sensor[40];
    char body[96];
    sensor_cmd_copy_token(sensor, sizeof(sensor), sensor_id);
    sensor_cmd_copy_token(body, sizeof(body), frame);
    int n = snprintf(out, out_len, "cmd,%lu,%lu,%d,%s,%s",
                     (unsigned long)unix_time, (unsigned long)id, ok ? 1 : 0, sensor, body);
    if (n < 0 || (size_t)n >= out_len) return -1;
    return n;
}

inline void sensor_cmd_report_set(SensorCmdReport* report, uint32_t id, bool ok, const char* err, const char* rsp) {
    if (!report) return;
    memset(report, 0, sizeof(*report));
    report->id = id;
    report->ok = ok ? 1 : 0;
    report->pending = true;
    if (!ok && err && err[0]) {
        strncpy(report->err, err, sizeof(report->err) - 1);
    }
    if (rsp && rsp[0]) {
        strncpy(report->rsp, rsp, sizeof(report->rsp) - 1);
    }
}

// Copies sc_id, sc_ok, sc_err and sc_rsp once, then clears the pending report.
inline bool sensor_cmd_take_report(SensorCmdReport* report, SensorCmdAttach* out) {
    if (!report || !out) return false;
    memset(out, 0, sizeof(*out));
    if (!report->pending) return false;
    out->include = true;
    out->id = report->id;
    out->ok = report->ok ? 1 : 0;
    memcpy(out->err, report->err, sizeof(out->err));
    memcpy(out->rsp, report->rsp, sizeof(out->rsp));
    memset(report, 0, sizeof(*report));
    return true;
}

uint32_t sensor_cmd_finished_id();
void sensor_cmd_execute(uint32_t cmd_id, const char* sensor_id, const char* frame);
// Saves sc_ok 0 and sc_err late. Does not mark CMD_ID finished.
void sensor_cmd_note_late(uint32_t cmd_id);
bool sensor_cmd_load_pending(SensorCmdAttach* out);
