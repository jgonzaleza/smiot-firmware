#include "sensor_cmd.h"

#include "firmware_context.h"
#include "app_utils.h"
#include "sensors.h"
#include "time_manager.h"

#include <ModbusMaster.h>
#include <SD.h>

static const int SENSOR_CMD_SLOTS = 8;
static const int SENSOR_CMD_ID_BYTES = 40;

static int load_sensor_slots(SensorCmdSlot* slots, char ids[][SENSOR_CMD_ID_BYTES], int cap) {
    int n = namespaces_size;
    if (n > cap) n = cap;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        ids[i][0] = '\0';
        slots[i].id = ids[i];
        slots[i].baud = 0;
        if (!preferences.begin(namespace_id[i].c_str(), true)) continue;
        String stored = preferences.getString("SENSOR_ID", "");
        int baud = preferences.getInt("BAUD_RATE", 0);
        preferences.end();
        size_t len = stored.length();
        if (len >= (size_t)SENSOR_CMD_ID_BYTES) len = SENSOR_CMD_ID_BYTES - 1;
        memcpy(ids[i], stored.c_str(), len);
        ids[i][len] = '\0';
        slots[i].baud = baud;
    }
    return n;
}

// mark_finished stores cmdid. The awake skip passes false so CMD_ID stays open.
static void save_sensor_cmd(const SensorCmdReport* report, bool mark_finished) {
    if (!report) return;
    if (!preferences.begin("devcfg", false)) return;
    if (mark_finished) preferences.putUInt("cmdid", report->id);
    preferences.putUInt("sc_id", report->id);
    preferences.putUChar("sc_ok", (uint8_t)(report->ok ? 1 : 0));
    preferences.putString("sc_err", report->err);
    preferences.putString("sc_rsp", report->rsp);
    preferences.putBool("sc_pend", true);
    preferences.end();
}

static void append_sensor_cmd_line(uint32_t cmd_id, const char* sensor_id, const char* frame) {
    if (!ensure_sd_active()) return;
    if (!SD.exists("/log")) SD.mkdir("/log");
    char line[180];
    uint32_t unix_time = (uint32_t)get_current_timestamp();
    if (sensor_cmd_format_sd_line(line, sizeof(line), unix_time, cmd_id, 1, sensor_id, frame) < 0) return;
    File file = SD.open("/log/cmd.log", FILE_APPEND);
    if (!file) return;
    file.println(line);
    file.close();
}

// Same bus setup as the sensor read. ModbusMaster adds the CRC.
// Function 06 is writeSingleRegister. Function 16 is setTransmitBuffer
// plus writeMultipleRegisters. ModbusMaster 2.0.1 does not copy a write
// reply into the response buffer, so sc_rsp stays empty for these writes.
static uint8_t write_sensor_frame(int baud, const SensorCmdFrame* frame) {
    ModbusMaster end_node;
    Serial485.begin(baud, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    delay(50);
    end_node.begin(frame->addr, Serial485);
    delay(500);
    end_node.clearResponseBuffer();
    end_node.clearTransmitBuffer();

    uint8_t result = SENSOR_CMD_MB_INVALID_FUNCTION;
    if (frame->function == 0x06) {
        result = end_node.writeSingleRegister(frame->reg, frame->values[0]);
    } else if (frame->function == 0x10) {
        for (uint16_t i = 0; i < frame->quantity && i < SENSOR_CMD_MAX_REGS; i++) {
            end_node.setTransmitBuffer((uint8_t)i, frame->values[i]);
        }
        result = end_node.writeMultipleRegisters(frame->reg, frame->quantity);
    }

    end_node.clearResponseBuffer();
    end_node.clearTransmitBuffer();
    delay(50);
    Serial485.flush();
    delay(50);
    Serial485.end();
    delay(50);
    return result;
}

uint32_t sensor_cmd_finished_id() {
    if (!preferences.begin("devcfg", true)) return 0;
    uint32_t id = preferences.getUInt("cmdid", 0);
    preferences.end();
    return id;
}

bool sensor_cmd_load_pending(SensorCmdAttach* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!preferences.begin("devcfg", false)) return false;
    if (!preferences.getBool("sc_pend", false)) {
        preferences.end();
        return false;
    }
    SensorCmdReport report;
    memset(&report, 0, sizeof(report));
    report.pending = true;
    report.id = preferences.getUInt("sc_id", 0);
    report.ok = preferences.getUChar("sc_ok", 0) ? 1 : 0;
    String err = preferences.getString("sc_err", "");
    String rsp = preferences.getString("sc_rsp", "");
    strncpy(report.err, err.c_str(), sizeof(report.err) - 1);
    strncpy(report.rsp, rsp.c_str(), sizeof(report.rsp) - 1);
    SensorCmdAttach attach;
    bool took = sensor_cmd_take_report(&report, &attach);
    if (took) preferences.putBool("sc_pend", false);
    preferences.end();
    if (!took) return false;
    *out = attach;
    return true;
}

void sensor_cmd_note_late(uint32_t cmd_id) {
    SensorCmdReport report;
    sensor_cmd_report_set(&report, cmd_id, false, "late", NULL);
    save_sensor_cmd(&report, false);
}

void sensor_cmd_execute(uint32_t cmd_id, const char* sensor_id, const char* frame_text) {
    char id_buf[SENSOR_CMD_SLOTS][SENSOR_CMD_ID_BYTES];
    SensorCmdSlot slots[SENSOR_CMD_SLOTS];
    int count = load_sensor_slots(slots, id_buf, SENSOR_CMD_SLOTS);
    SensorCmdChoice choice;
    sensor_cmd_choose(sensor_id, slots, count, &choice);

    SensorCmdFrame frame;
    memset(&frame, 0, sizeof(frame));
    if (choice.send) sensor_cmd_parse(frame_text ? frame_text : "", &frame);

    const char* err = NULL;
    bool write_ok = false;
    if (!choice.send) {
        err = choice.err ? choice.err : "unknown_sensor";
    } else if (!frame.send) {
        err = frame.err ? frame.err : "bad_frame";
    } else {
        init_peripherals();
        turn_sensors_on();
        start_time = millis();
        wait_for_sensors();
        feed_watchdog();
        uint8_t result = write_sensor_frame(choice.baud, &frame);
        feed_watchdog();
        if (result == SENSOR_CMD_MB_SUCCESS) write_ok = true;
        else err = sensor_cmd_result_error((int)result);
        turn_sensors_off();
    }

    SensorCmdReport report;
    sensor_cmd_report_set(&report, cmd_id, write_ok, err, NULL);
    save_sensor_cmd(&report, true);

    if (sensor_cmd_restarts_on(write_ok)) {
        append_sensor_cmd_line(cmd_id, sensor_id, frame_text);
        perform_restart();
    }
}
