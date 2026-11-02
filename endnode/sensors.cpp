#include "firmware_context.h"
#include "logger.h"
#include "app_utils.h"
#include "network.h"
#include <ModbusMaster.h>
#include <ArduinoJson.h>
#include "sensors.h"
#include "gps_module.h"
#include "power_policy.h"
#include "queue_manager.h"
#include "time_manager.h"
#include "ota_guard.h"
#include "csv_parse.h"
#include "variant.h"
#include <Update.h>
#include <esp_task_wdt.h>

void init_peripherals() {
    local_logger("Initializing RS485 . . .");    
    local_logger("RS485_EN_PIN . . .");
    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, HIGH);

    local_logger("PIN_5V_EN . . .");
    pinMode(PIN_5V_EN, OUTPUT);
    digitalWrite(PIN_5V_EN, HIGH);

    local_logger("#### RS485 Init Completed ####");
}
void wait_for_sensors(){
    unsigned long phase_start = millis();
    unsigned long elapsed_time = millis() - start_time;
    unsigned long cap = TIME_TO_READ_SENSOR;
    if (cap < TTR_SENSOR_MIN_MS) cap = TTR_SENSOR_MIN_MS;
    if (cap > TTR_SENSOR_MAX_MS) cap = TTR_SENSOR_MAX_MS;
    while (elapsed_time < cap) {
        if (phase_deadline_hit(phase_start, cap + 1000UL)) {
            local_logger("wait_for_sensors deadline");
            break;
        }
        local_logger("Waiting for minimal sensor time . . .");
        delay(1000);
        feed_watchdog();
        elapsed_time = millis() - start_time;
    }
    #ifdef DEBUG
        local_logger("######>>>>>> Slapsed Time: ");
        Serial.println(elapsed_time);
    #endif
    
}
void checkAndResetRainSensor() {
    long currentTimestampUtc = (long)get_current_timestamp();
    if (currentTimestampUtc <= 0) {
        return;
    }

    // Local offset stays the compiled Costa Rica value until the COUNTRY setting.
    long localTimestamp = currentTimestampUtc + timezone_offset_sec;
    if (localTimestamp < 0) return;

    long currentDayNumber = localTimestamp / 86400;
    long secOfDay = localTimestamp % 86400;
    const long MIDNIGHT_WINDOW_SEC = 30L * 60L;

    if (lastResetDayNumber < 0) {
        lastResetDayNumber = currentDayNumber;
        rtc_persist_commit();
        local_logger("Rain day initialized to ", currentDayNumber);
        return;
    }

    if (currentDayNumber == lastResetDayNumber) return;

    long previousDay = lastResetDayNumber;
    lastResetDayNumber = currentDayNumber;
    rtc_persist_commit();
    // Pulse the relay only near local midnight. A later wake must not wipe the day's rain.
    if (secOfDay >= 0 && secOfDay <= MIDNIGHT_WINDOW_SEC) {
        reset_sensor();
        local_logger("Rain sensor reset near local midnight. Old day ", previousDay, " new day ", currentDayNumber);
    } else {
        local_logger("Day changed outside the midnight window. Rain relay not pulsed.");
    }
}
String get_sensor_payload(String sensor_id, int baud_rate, int sensor_address, int start_holding_register, int registers_to_read, String value_names[], String value_types[], int value_names_size, bool has_types) {

    uint32_t j; 
    uint8_t result;
    ModbusMaster end_node;
    JsonDocument doc;
    String return_value = "{\"sensor\": -1}";

    Serial485.begin(baud_rate, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    delay(50);
    end_node.begin(sensor_address, Serial485);
    delay(500);

    result = end_node.readHoldingRegisters(start_holding_register, registers_to_read);
    
    if (result == end_node.ku8MBSuccess) {        
        
        int buffer_index = 0; // Índice independiente para la memoria Modbus

        for (j = 0; j < value_names_size; j++) {
            String current_name = value_names[j];
            
            // Lógica por defecto: Si no hay tipos declarados, asumimos INTEGER
            String current_type = "INTEGER"; 
            if (has_types) {
                current_type = value_types[j];
            }

            // Procesamiento según el tipo
            if (current_type == "FLOAT32_ABCD" || current_type == "FLOAT32_CDAB") {
                
                uint16_t word1 = end_node.getResponseBuffer(buffer_index);
                uint16_t word2 = end_node.getResponseBuffer(buffer_index + 1);
                
                
                uint32_t combined_bits = 0;
                
                // Aplicamos el clásico "Word Swap" y corrimiento de bits
                if (current_type == "FLOAT32_CDAB") {
                    combined_bits = ((uint32_t)word2 << 16) | word1; 
                } else { // FLOAT32_ABCD
                    combined_bits = ((uint32_t)word1 << 16) | word2;
                }
                
                // La técnica definitiva a prueba de compiladores (Strict Aliasing)
                float float_result = 0.9f;
                memcpy(&float_result, &combined_bits, sizeof(float_result));

                doc[current_name] = float_result;
                buffer_index += 2; // Consumimos 2 registros (32 bits)

            } else {
                // Caso "INTEGER" o cualquier otro valor no reconocido (Fallback de seguridad)
                uint16_t raw_value = end_node.getResponseBuffer(buffer_index);
                doc[current_name] = raw_value;
                buffer_index += 1; // Consumimos 1 registro (16 bits)
            }
        }
    } else {
        Serial.println("########### Sensor RS485 Read Failed ###########");
        doc["err"] = "ERROR";
        doc["res"] = result;
        doc["addr"] = sensor_address;
        doc["reg"] = start_holding_register;
        doc["len"] = registers_to_read;
        doc["baud"] = baud_rate;
        doc["sz"] = value_names_size;
        doc["id"] = sensor_id;
        doc["val"] = value_names[0];        
    }
    
    serializeJson(doc, return_value);

    end_node.clearResponseBuffer();
    end_node.clearTransmitBuffer();
    delay(50);
    Serial485.flush();
    delay(50);
    Serial485.end();
    delay(50);

    return return_value;
}
bool get_sensor_configuration(bool show_status) {

    JsonDocument parameters_payload;
    String device_id = String(ESP.getEfuseMac());

    // Initialize HTTPS
    if (show_status) {
        blink_led(255,255,255, 500);
        comm.https_begin();
        blink_led(255,255,255, 500);
    } else {
        comm.https_begin();
    }


    // Set GET URT
    if (!comm.https_set_url(CONFIGURATION_SERVER_URL)) {
        Serial.println("Failed to set the URL. Please check the validity of the URL!");
        comm.https_end();
        if (show_status) {
            blink_led(255,0,0,10000);
        }        
        return false;
    }
    if (show_status) {
        blink_led(255,255,255,500);
    }

    // Send POST request
    comm.https_add_header("X-API-Key", CONFIGURATION_SERVER_KEY);
    int httpCode = 0;
    String post_payload = "{\"device_type\": ";
    post_payload.concat(active_device_type);
    post_payload.concat(", \"device_id\": \"");
    post_payload.concat(device_id);
    post_payload.concat("\"}");
    httpCode = comm.https_post(post_payload);
    if (show_status) {
        blink_led(255,255,255,500);
    }
    if (httpCode != 200) {
        Serial.print("HTTP get failed ! error code = "); 
        Serial.println(httpCode);         
        comm.https_end();
        local_logger("HTTP get failed GET UPDATE PARAMETERS");
        if (show_status) {
            blink_led(255,0,0,10000);
        }
        return false;
    }

    if (show_status) {
        blink_led(255,255,255,500);
    }
    String body = comm.https_body();
    if (show_status) {
        blink_led(255,255,255,500);
    }
    #ifdef DEBUG
        Serial.print("HTTP body : ");
        Serial.println(body);
        delay(5000);
    #endif

    //Conver the String to JSON Document
    DeserializationError config_error = deserializeJson(parameters_payload, body);
    if (!config_error && !parameters_payload["DEVICE_TYPE"].isNull()) {
        int role = parameters_payload["DEVICE_TYPE"].as<int>();
        if (role >= 1 && role <= 9) {
            active_device_type = role;
            preferences.begin("devcfg", false);
            preferences.putInt("dtype", role);
            preferences.end();
        }
    }
    if (!config_error && !parameters_payload["COUNTRY"].isNull()) {
        JsonObject country = parameters_payload["COUNTRY"].as<JsonObject>();
        long tz = country["tz_offset"] | (long)timezone_offset_sec;
        if (tz < -50400) tz = -50400;
        if (tz > 50400) tz = 50400;
        timezone_offset_sec = tz;
        const char* code = "CR";
        if (!country["code"].isNull()) {
            const char* parsed = country["code"].as<const char*>();
            if (parsed && parsed[0]) code = parsed;
        }
        JsonArray plmn = country["plmn"].as<JsonArray>();
        preferences.begin("devcfg", false);
        preferences.putInt("tz", (int)timezone_offset_sec);
        preferences.putString("ctry", code);
        int slot = 0;
        for (JsonVariant item : plmn) {
            if (slot >= 4) break;
            const char* text = item.as<const char*>();
            if (!text || strlen(text) == 0 || strlen(text) > 7) continue;
            strncpy(plmn_table[slot], text, 7);
            plmn_table[slot][7] = '\0';
            preferences.putString(("p" + String(slot)).c_str(), plmn_table[slot]);
            slot++;
        }
        preferences.end();
    }
    if (show_status) {
        blink_led(255,255,255,500);
    }
    #ifdef DEBUG
        String str_sensors = parameters_payload["sensors"];
        Serial.println("SENSORS CONFIGURATION: ");
        Serial.println(str_sensors);
        delay(2000);
    #endif
    JsonArray sensors = parameters_payload["sensors"];
    if (sensors.isNull() || sensors.size() <= 0 || sensors.size() > (size_t)namespaces_size) {
        comm.https_end();
        if (show_status) {
            blink_led(255,0,0,10000);
        }
        return false;
    }

    struct PendingSensor {
        String sensor_id;
        int is_async;
        int trig_reg;
        int trig_val;
        int stat_reg;
        int ready_val;
        int warmup_val;
        uint32_t max_wait_ms;
        int address;
        int start_reg;
        int regs;
        int baud;
        String value_names;
        String value_types;
        bool has_types;
    };

    PendingSensor pending[8];
    int pending_count = 0;
    bool config_valid = true;
    for (JsonVariant sensor_config : sensors) {
        if (pending_count >= namespaces_size) {
            config_valid = false;
            break;
        }
        PendingSensor& item = pending[pending_count];
        item.sensor_id = sensor_config["SENSOR_ID"].as<String>();
        item.sensor_id.trim();
        item.is_async = sensor_config["IS_ASYNC"] | 0;
        if (item.is_async != 0 && item.is_async != 1) item.is_async = 0;
        item.address = sensor_config["SENSOR_ADDRESS"] | 0;
        item.start_reg = sensor_config["START_HOLDING_REGISTER"] | 0;
        item.regs = sensor_config["REGISTERS_TO_READ"] | 0;
        item.baud = sensor_config["BAUD_RATE"] | 9600;
        item.value_names = sensor_config["VALUE_NAMES"].as<String>();
        item.has_types = sensor_config["VALUE_TYPES"].is<String>();
        item.value_types = item.has_types ? sensor_config["VALUE_TYPES"].as<String>() : String("");
        item.trig_reg = sensor_config["TRIG_REG"] | 0;
        item.trig_val = sensor_config["TRIG_VAL"] | 1;
        item.stat_reg = sensor_config["STAT_REG"] | 1;
        item.ready_val = sensor_config["READY_VAL"] | 3;
        item.warmup_val = sensor_config["WARMUP_VAL"] | 1;
        item.max_wait_ms = sensor_config["MAX_WAIT_MS"] | 60000;
        if (item.max_wait_ms < 1000) item.max_wait_ms = 1000;
        if (item.max_wait_ms > 180000) item.max_wait_ms = 180000;

        char name_tokens[CSV_MAX_TOKENS][CSV_TOKEN_MAX + 1];
        bool names_truncated = false;
        int name_count = csv_parse_tokens(item.value_names.c_str(), name_tokens, CSV_MAX_TOKENS, &names_truncated);
        bool types_ok = true;
        if (item.has_types) {
            char type_tokens[CSV_MAX_TOKENS][CSV_TOKEN_MAX + 1];
            bool types_truncated = false;
            int type_count = csv_parse_tokens(item.value_types.c_str(), type_tokens, CSV_MAX_TOKENS, &types_truncated);
            types_ok = !types_truncated && type_count == name_count;
        }
        if (item.sensor_id.length() == 0 || item.sensor_id.length() > 32
            || item.address < 1 || item.address > 247
            || item.start_reg < 0 || item.start_reg > 65535
            || item.regs < 1 || item.regs > 32
            || item.baud < 1200 || item.baud > 115200
            || name_count < 1 || names_truncated || !types_ok) {
            config_valid = false;
            break;
        }
        pending_count++;
        feed_watchdog();
    }

    if (!config_valid || pending_count == 0) {
        local_logger("Sensor config rejected. Previous configuration kept.");
        parameters_payload.clear();
        comm.https_end();
        if (show_status) blink_led(255, 0, 0, 10000);
        return false;
    }

    // Swap only after the whole document validated.
    has_async_sensors = false;
    for (int i = 0; i < namespaces_size; i++) {
        preferences.begin(namespace_id[i].c_str(), false);
        preferences.clear();
        if (i < pending_count) {
            PendingSensor& item = pending[i];
            preferences.putString("SENSOR_ID", item.sensor_id);
            preferences.putInt("IS_ASYNC", item.is_async);
            if (item.is_async == 1) {
                has_async_sensors = true;
                preferences.putInt("TRIG_REG", item.trig_reg);
                preferences.putInt("TRIG_VAL", item.trig_val);
                preferences.putInt("STAT_REG", item.stat_reg);
                preferences.putInt("READY_VAL", item.ready_val);
                preferences.putInt("WARMUP_VAL", item.warmup_val);
                preferences.putUInt("MAX_WAIT_MS", item.max_wait_ms);
            }
            preferences.putInt("SENSOR_ADDRESS", item.address);
            preferences.putInt("START_HOLD_REG", item.start_reg);
            preferences.putInt("REGS_TO_READ", item.regs);
            preferences.putInt("BAUD_RATE", item.baud);
            preferences.putString("VALUE_NAMES", item.value_names);
            if (item.has_types) preferences.putString("VALUE_TYPES", item.value_types);
        }
        preferences.end();
        feed_watchdog();
        if (show_status) blink_led(255, 255, 255, 500);
    }

    parameters_payload.clear();
    comm.https_end();
    if (show_status) {
        blink_led(255,255,255,500);
    }
    return true;
}
bool sensor_config_present() {
    for (int i = 0; i < namespaces_size; i++) {
        preferences.begin(namespace_id[i].c_str(), true);
        bool present = preferences.isKey("SENSOR_ID");
        preferences.end();
        if (present) return true;
    }
    return false;
}
void clear_sensors_configuration() {
    for (int i=0;i<namespaces_size;i++) {
        #ifdef DEBUG
            Serial.println("CLEAR NAMESPACE:");            
            Serial.println(namespace_id[i].c_str());
            delay(500);
        #endif
        preferences.begin(namespace_id[i].c_str());
        if (preferences.isKey("SENSOR_ID")) {
            preferences.remove("SENSOR_ID");
        }
        preferences.clear();
        preferences.end();
    }
}
void prepare_async_sensors() {

    if (!has_async_sensors) {        
        local_logger("Skipping async preparation. No async sensors configured.");
        return; 
    }

    uint32_t max_wait_required = 0;
    bool waiting_needed = false;

    // Check if we have async sensors and trigger them, also calculate the max wait time required
    for (int i = 0; i < namespaces_size; i++) {
        preferences.begin(namespace_id[i].c_str(), true); // Solo lectura
        
        // Check if the sensor configuration exists and is marked as async
        if (preferences.getInt("IS_ASYNC", 0) == 1) {
            int baud      = preferences.getInt("BAUD_RATE", 9600);
            int addr      = preferences.getInt("SENSOR_ADDRESS", 9);
            int trig_reg  = preferences.getInt("TRIG_REG", 0);
            int trig_val  = preferences.getInt("TRIG_VAL", 1);
            int stat_reg  = preferences.getInt("STAT_REG", 1);
            int warmup_val = preferences.getInt("WARMUP_VAL", 2);
            int ready_val = preferences.getInt("READY_VAL", 3);
            uint32_t wait = preferences.getUInt("MAX_WAIT_MS", 68000);

            // Bus control
            Serial485.begin(baud, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
            delay(50);
            ModbusMaster end_node;
            end_node.begin(addr, Serial485);
            delay(500);

            // CONSULTA DE ESTADO
            uint8_t result = end_node.readHoldingRegisters(stat_reg, 1);
            if (result == end_node.ku8MBSuccess) {
                int status = end_node.getResponseBuffer(0);
                if (status < warmup_val) {
                    // Trigger the sensor to start the measurement(Warm/Stabilization)
                    add_sensor_error("[TRIG_REG]:[" + String(trig_reg) + "]");
                    add_sensor_error("[TRIG_VAL]:[" + String(trig_val) + "]");
                    uint8_t write_res = end_node.writeSingleRegister(trig_reg, trig_val);                    
                    if (write_res != end_node.ku8MBSuccess) {
                        add_sensor_error("[ASYNC_TRIG_FAIL]:[" + String(write_res) + "]");
                    } else {
                        add_sensor_error("[ASYNC_TRIG_OK]");
                    }
                    waiting_needed = true;
                    if (wait > max_wait_required) max_wait_required = wait;
                } else if (status == warmup_val) {
                    // Sensor is stabilizing waiting is needed
                    waiting_needed = true;
                    if (wait > max_wait_required) max_wait_required = wait;
                } 
                add_sensor_error("[ASYNC_WAITING_NEEDED]:[" + String(waiting_needed) + "]");
                add_sensor_error("[ASYNC_WAITING_VALUE]:[" + String(max_wait_required) + "]");
                add_sensor_error("[ASYNC_STATUS]:[" + String(status) + "]");
            } else {
                local_logger("Failed to read sensor state:", namespace_id[i].c_str());
                add_sensor_error("[ASYNC_GET_STATE_ERROR]:[" + String(result) + "]");
            }
            
            end_node.clearResponseBuffer();
            end_node.clearTransmitBuffer();
            Serial485.flush();
            delay(50);
            Serial485.end();
            delay(100);
        }
        preferences.end();
        delay(100);
    }

    // Active wait and check if sensors are ready before read them
    bool all_ready = false;
    int status = 0;
    if (waiting_needed) {
        uint32_t l_start_time = millis();        

        while ((millis() - l_start_time) < max_wait_required && !all_ready) {
            feed_watchdog();
            delay(3000);

            all_ready = true;
            for (int i = 0; i < namespaces_size; i++) {
                preferences.begin(namespace_id[i].c_str(), true);
                if (preferences.getInt("IS_ASYNC", 0) == 1) {
                    int baud      = preferences.getInt("BAUD_RATE", 9600);
                    int addr      = preferences.getInt("SENSOR_ADDRESS", 9);
                    int stat_reg  = preferences.getInt("STAT_REG", 1);
                    int ready_val = preferences.getInt("READY_VAL", 3);

                    Serial485.begin(baud, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
                    delay(50);
                    ModbusMaster end_node;
                    end_node.begin(addr, Serial485);
                    delay(500);

                    uint8_t result = end_node.readHoldingRegisters(stat_reg, 1);
                    if (result == end_node.ku8MBSuccess) {
                        status = end_node.getResponseBuffer(0);
                        if (status != ready_val) {
                            all_ready = false;
                        } else{
                            add_sensor_error("[ASYNC_SENSOR_READY]:[" + String(status) + "]");
                        }
                    } else {
                        local_logger("Failed to read sensor state while waiting:", namespace_id[i].c_str());
                        add_sensor_error("[ASYNC_STATE_ERROR_WAITING]:[" + String(result) + "]");
                        all_ready = false;
                    }

                    end_node.clearResponseBuffer();
                    end_node.clearTransmitBuffer();
                    Serial485.flush();
                    delay(50);
                    Serial485.end();
                    delay(100);
                }
                preferences.end();
            }
        }
    }
    if (waiting_needed && !all_ready) {
        local_logger("Critical timeout: the sensor did not reach ready.");
        add_sensor_error("[ASYNC_TIMEOUT]");
        add_sensor_error("[ASYNC_LAST_STATUS]:[" + String(status) + "]");        
    }    
}
String get_final_sensor_payload(){

    JsonDocument doc;
    String final_sensor_payload;
    
    unsigned int rate = READING_RATE;
    if (rate < R_RATE_MIN) rate = R_RATE_MIN;
    if (rate > R_RATE_MAX) rate = R_RATE_MAX;
    for (unsigned int k = 0; k < rate; k++) {
        for (int i=0;i<namespaces_size;i++) {
            #ifdef DEBUG
                Serial.println(". . . get_final_sensor_payload . . .");
                Serial.println("NAMESPACE: ");
                Serial.println(namespace_id[i].c_str());
                delay(100);
            #endif
            preferences.begin(namespace_id[i].c_str(), true);
            
            if (preferences.isKey("SENSOR_ID")) {
                String sensor_id = preferences.getString("SENSOR_ID");
                int sensor_address = preferences.getInt("SENSOR_ADDRESS");
                int baud_rate = preferences.getInt("BAUD_RATE");
                if (baud_rate == 0) {
                    baud_rate = DEFAULT_BAUD_RATE;
                }
                int start_holding_register = preferences.getInt("START_HOLD_REG");
                int registers_to_read = preferences.getInt("REGS_TO_READ");
                
                String str_value_names = preferences.getString("VALUE_NAMES");
                String str_value_types = preferences.getString("VALUE_TYPES", "");
                
                #ifdef DEBUG
                    Serial.println("VALUE_NAMES: ");
                    Serial.println(str_value_names);
                    Serial.println("VALUE_TYPES: ");
                    Serial.println(str_value_types);            
                    delay(100);
                #endif
                
                char name_tokens[CSV_MAX_TOKENS][CSV_TOKEN_MAX + 1];
                char type_tokens[CSV_MAX_TOKENS][CSV_TOKEN_MAX + 1];
                bool names_truncated = false;
                bool types_truncated = false;
                int value_names_size = csv_parse_tokens(str_value_names.c_str(), name_tokens, CSV_MAX_TOKENS, &names_truncated);
                int value_types_size = 0;
                if (str_value_types.length() > 0) {
                    value_types_size = csv_parse_tokens(str_value_types.c_str(), type_tokens, CSV_MAX_TOKENS, &types_truncated);
                }
                if (names_truncated || types_truncated) {
                    add_sensor_error("[CSV_TRUNC]");
                }
                String value_names[CSV_MAX_TOKENS];
                String value_types[CSV_MAX_TOKENS];
                for (int t = 0; t < value_names_size; t++) value_names[t] = name_tokens[t];
                bool has_types = value_types_size > 0;
                for (int t = 0; t < CSV_MAX_TOKENS; t++) {
                    value_types[t] = (t < value_types_size) ? String(type_tokens[t]) : String("INTEGER");
                }

                // Llamada al método con la bandera has_types
                String sensor_payload = get_sensor_payload(sensor_id, baud_rate, sensor_address, start_holding_register, registers_to_read, value_names, value_types, value_names_size, has_types);
                
                String str_sensor_id = sensor_id + "_";
                str_sensor_id.concat(k);
                doc[str_sensor_id] = serialized(sensor_payload);
                
                delay(250);
            }
            preferences.end();                        
        }
        unsigned long gap_sec = TIME_BETWEEN_READINGS;
        if (gap_sec > TB_READINGS_MAX_SEC) gap_sec = TB_READINGS_MAX_SEC;
        for (unsigned long sec = 0; sec < gap_sec; sec++) {
            delay(1000);
            feed_watchdog();
        }
    }

    serializeJson(doc, final_sensor_payload);

    local_logger("Final Sensor Payload: ");
    local_logger(final_sensor_payload);

    return final_sensor_payload;
}

String sensors_sample(bool* configured) {
    (void)configured;
    checkAndResetRainSensor();
    init_peripherals();
    rtc_panic_flag = 1;
    rtc_persist_commit();
    turn_sensors_on();
    start_time = millis();
    prepare_async_sensors();
    feed_watchdog();
    wait_for_sensors();
    feed_watchdog();
    String payload = get_final_sensor_payload();
    feed_watchdog();
    return payload;
}
