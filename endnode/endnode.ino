/**
 * Smart IoT End Node Firmware 4.0.4
 * Board: Lilygo A7608X-S3
 * Flow: Init -> HTTPS Config/Update -> Read Sensors  -> MQTT Upload -> Store RTC( SD optional) -> Deep Sleep
 * Date: Junio 2026
 * Author: jgonzaleza
 * Changelog: v200 - Initial version
 * Change Log: v201 - Added automatic reset rain sensor before midnight.
 * Change Log: v202 - Corrected automatic reset rain sensor logic to properly handle edge cases around midnight and ensure consistent behavior.
 * Change Log: v205 - Update for SD Card Diagnostics.
 * Change Log: v300 - Refactor for better modularity. 
 *                    Change queue logic to use RTC memory and SD Card as backup only for failed uploads. 
 *                    Add Batch message sending for optimized network usage.
 *                    Add support for asynchronous sensors with configurable trigger and status registers.
 *                    Add Network change in case of repeated upload failures.(Zombie Network Recovery)
 *                    Add auto restart when sensors payload becomes null for consecutive measurements.
 *                    Add support for sensors with floating point values.
 *                    Add support for network timeouts during firmware updates.
 * Change Log: v310 - RTC state survives resets. Queue bounds, MQTT parameter clamps,
 *                    watchdog deadlines, crash-safe SD offset, error ring.
 * Change Log: 4.0.1 - Deep-sleep modem bring-up, radio-skip accounting,
 *                    confirm after two sleep uplinks, network step log.
 * Change Log: 4.0.2 - Refuse placeholder certificates. Reload the modem
 *                    certificates when the compiled material changes, and
 *                    once per wake when TLS connect returns 32, 33, or 34.
 * Change Log: 4.0.3 - Finish AT+CPOF before deep sleep, keep a minimum
 *                    sleep after a long wake, and cold-reset when AT
 *                    answers but the SIM does not.
 * Change Log: 4.0.4 - Deep sleep powers the modem on from a confirmed-off
 *                    state (1000ms PWRKEY retry, not 200ms). Hold PWRKEY
 *                    low across sleep. AT reads honor their timeout.
 *                    First boot stops at the awake budget.
 */

#define TINY_GSM_RX_BUFFER 1024 // Set RX buffer to 1Kb


// See all AT commands, if wanted
//#define DUMP_AT_COMMANDS
//#define DEBUG true

// Include Headers
#include "tinygsm_yield.h"
#include "utilities.h"     // Board specific pin definitions
#include "error_codes.h"
#include "logger.h"
#include "app_utils.h"
#include "custom_pins.h"
#include "time_manager.h"
#include "queue_manager.h"
#include "csv_parse.h"
#include "param_limits.h"
#include "param_flags.h"
#include "power_policy.h"
#include "sketch_config.h"
#include "wake_policy.h"
#include "variant.h"

// External Libraries
#include <ModbusMaster.h>
#include <TinyGsmClient.h>
#include <HardwareSerial.h>
#include <esp_adc_cal.h>
#include <ArduinoJson.h>
#include <ctime> 
#include <Update.h>
#include <driver/rtc_io.h>
#include <string.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <vector>

// Security Certificates
#include "certs/AWSClientCertificate.h"
#include "certs/AWSClientPrivateKey.h"
#include "certs/AmazonRootCA.h"
#include "cert_guard.h"
#include "CommunicationManager.h"
#include "CellularCommunicationClient.h"
#ifdef FEATURE_WIFI
#include "WifiCommunicationClient.h"
#endif

// Production builds leave DEBUG unset, so console prints compile to a dead branch.
#ifndef DEBUG
#define Serial if (false) Serial
#endif


// ================= CONSTANTS & CONFIG ================= //

// Network. DEFAULT_NETWORK_APN and SIM_PREFIXES live in sketch_config.h.
String global_apn = DEFAULT_NETWORK_APN;
String sim_label = "";
String sim_iccid = "";
String registered_plmn = "";
RTC_DATA_ATTR bool sim_unknown_reported = false;
RTC_DATA_ATTR uint32_t net_sticky_since = 0;
char plmn_table[4][8] = {"AUTO", "71203", "71204", "71201"};
long timezone_offset_sec = -21600;
#define MAX_CONSECUTIVE_FAILURES 12 // Max number of consecutive failures before giving up and restarting
#define MAX_NETWORK_FAILURES 3 // Max consecutive network failures before we trigger a network change


// AWS IOT MQTT
const char *broker = "a2llv4oq369v2i-ats.iot.us-east-1.amazonaws.com";
const uint16_t broker_port = 8883;
const char *client_id_prefix = "A7608-S3-01-005";
const uint8_t client_index = 0;
const uint8_t mqtt_client_id = 1;
String publish_topic_prefix = "smartiotcr/"; // Topics for MQTT publishing
String subscribe_topic_prefix = "smartiotcr/parameters/v2/"; // Topic to subscribe for parameters

// API Gateway URLs
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#include "firmware_update_request.h"
#ifndef CHECK_FIRMWARE_KEY
#define CHECK_FIRMWARE_KEY ""
#endif
#ifndef FIRMWARE_UPDATE_KEY
#define FIRMWARE_UPDATE_KEY ""
#endif
#ifndef CONFIGURATION_SERVER_KEY
#define CONFIGURATION_SERVER_KEY ""
#endif
const char *BASE_FIRMWARE_UPDATE_URL = "https://alxv44fafe.execute-api.us-east-1.amazonaws.com/dev/";
const char *CONFIGURATION_SERVER_URL =  "https://irwouyd9p6.execute-api.us-east-1.amazonaws.com/pro/device_sensor_config"; // Configuration Server details

//Firmware Update Parameters
#define DEFAULT_CHECK_FIRMWARE_UPDATE 86400000L // Check every 24 hours
const uint32_t OTA_TIMEOUT = 30000L; // 30 seconds network timeout
RTC_DATA_ATTR unsigned long CHECK_FIRMWARE_UPDATE = DEFAULT_CHECK_FIRMWARE_UPDATE;
RTC_DATA_ATTR unsigned long firmware_running_time;


//Device Type ID
#ifndef DEFAULT_DEVICE_TYPE
#define DEFAULT_DEVICE_TYPE 2
#endif
// Compiled default. The running value comes from the backend config when present.
int active_device_type = DEFAULT_DEVICE_TYPE;
//Firmware version defined in platformio.ini, should be updated with every firmware change to allow for proper OTA updates
const int firmware_version = FIRMWARE_VERSION;
#include "ota_guard.h"
#include "ota_verify.h"
#include <mbedtls/sha256.h> 

// Timing (Seconds)
#define uS_TO_S_FACTOR 1000000ULL
#define DEFAULT_SLEEP_SEC 510 // Default sleep time between measurements (5 mins)
RTC_DATA_ATTR uint32_t TIME_TO_SLEEP_SEC = DEFAULT_SLEEP_SEC; // Time to sleep between measurements

// Queue and Storage
#define maximumReadings 2000 // The maximum number of readings that can be stored in the available space

//Sensor Default Config
#define DEFAULT_BAUD_RATE 9600
#define DEFAULT_TIME_TO_SLEEP       540
#define DEFAULT_TIME_TO_READ_SENSOR     12000L
#define DEFAULT_READING_COUNT           1
#define DEFAULT_TIME_BETWEEN_READINGS   1 // Time between readings in seconds
#define DEFAULT_READING_RATE            1 // How many readings
#define DEFAULT_CHECK_PARAMETERS_UPDATE 3600000L
#define DEFAULT_CHECK_GPS_UPDATE 86400000L
RTC_DATA_ATTR unsigned int TIME_TO_READ_SENSOR = DEFAULT_TIME_TO_READ_SENSOR;
RTC_DATA_ATTR unsigned int READING_COUNT = DEFAULT_READING_COUNT;
RTC_DATA_ATTR unsigned int TIME_BETWEEN_READINGS = DEFAULT_TIME_BETWEEN_READINGS;
RTC_DATA_ATTR unsigned int READING_RATE = DEFAULT_READING_RATE;
RTC_DATA_ATTR bool has_async_sensors = false; // Flag to indicate if any of the sensors are asynchronous                                                                                            

// SD Card Diagnostics Parameters
RTC_DATA_ATTR bool is_sdcard_ok = false; // Flag to determine if SD card is working properly, set in the diagnostics function and used to decide if we store data or not
RTC_DATA_ATTR char rtc_sd_error_log[256] = ""; // Log for SD card diagnostics, stored in RTC for persistence across reboots

// Sensor Diagnostics Parameters
RTC_DATA_ATTR char rtc_sensor_error_log[256] = ""; // Log for Sensor diagnostics, stored in RTC for persistence across reboots

// Init parameters. first_run, the panic flag, counters and the rain day
// live in RTC_NOINIT (see rtc_persist.h) so a restart does not wipe them.
unsigned long start_time = 0;
unsigned long g_wake_start_ms = 0;

//GPS update parameters
RTC_DATA_ATTR bool get_gps_data;
RTC_DATA_ATTR float local_lat2;
RTC_DATA_ATTR float local_lon2;
RTC_DATA_ATTR unsigned long gps_running_time;
RTC_DATA_ATTR unsigned long CHECK_GPS_UPDATE = DEFAULT_CHECK_GPS_UPDATE;


//Objects
HardwareSerial Serial485(2);
//EndNode Configuration
Preferences preferences;
#ifdef DUMP_AT_COMMANDS  // if enabled it requires the streamDebugger lib
#include <StreamDebugger.h>
StreamDebugger debugger(SerialAT, Serial);
TinyGsm modem(debugger);
#else
TinyGsm modem(SerialAT);
#endif
TinyGsmClient client(modem);
CellularCommunicationClient cell_client;
#ifdef FEATURE_WIFI
WifiCommunicationClient wifi_client;
#endif
CommunicationManager comm;
bool modem_was_powered = false;
uint32_t active_sleep_sec = DEFAULT_SLEEP_SEC;
uint8_t active_power_mode = POWER_NORMAL;
bool announce_power_mode = false;
int max_backlog_batches = 2;
String configured_apn = "";
bool configured_apn_set = false;
RTC_DATA_ATTR uint8_t radio_skip_remaining = 0;
RTC_DATA_ATTR uint32_t last_uplink_unix = 0;
RTC_DATA_ATTR uint8_t gps_fail_streak = 0;
RTC_DATA_ATTR int saved_power_mode = POWER_NORMAL;
float last_pack_volts = -1;
int last_rssi = -1;

// Global Flags
bool reset_rain_sensor = false; //Flag to reset rain sensor

//We only support 8 configuration for sensors
extern const int namespaces_size = 8;
String namespace_id[namespaces_size] = {"SENSOR_1", "SENSOR_2", "SENSOR_3", "SENSOR_4", "SENSOR_5", "SENSOR_6", "SENSOR_7", "SENSOR_8"};

bool gps_fix_ok(float lat, float lon);

// ================= FUNCTION DEFINITIONS ================= //


// Send Sensor Payload via MQTT
bool publish_sensor_payload(String payload) {
    
    bool result = false;
    if (comm.mqtt_connected()) {
        // If connected, send information          
        local_logger("Publish Current Data");
        String publish_topic = publish_topic_prefix + String(ESP.getEfuseMac());
        result = comm.mqtt_publish(client_index, publish_topic.c_str(), payload.c_str());
        if (result) {
            local_logger("Data Published!");
            local_logger("#### Payload Sent ####");
            //delay(1000);
        } else {
            local_logger("Data Publish Failed!");
        }        
    }
    return result;
}

// Initialize Peripherals

static uint32_t compiled_cert_hash = 0;
static bool compiled_hash_known = false;
static bool compiled_hash_stored = false;
static bool tls_reload_used = false;

static void arm_cert_upload() {
#ifdef ALLOW_PLACEHOLDER_CERTS
    modem.mqtt_block_cert_upload(true);
    return;
#else
    modem.mqtt_block_cert_upload(false);
    if (!compiled_hash_known) {
        compiled_cert_hash = cert_material_hash(AWSClientCertificate, AWSClientPrivateKey);
        compiled_hash_known = true;
    }
    bool have = false;
    uint32_t stored = 0;
    if (preferences.begin("mqttcert", true)) {
        have = preferences.isKey("hash");
        stored = have ? preferences.getUInt("hash", 0) : 0;
        preferences.end();
    }
    // Differing hash: delete and re-upload the three modem files once this wake.
    modem.mqtt_force_cert_reload(!have || stored != compiled_cert_hash);
#endif
}

static void remember_cert_hash() {
#ifdef ALLOW_PLACEHOLDER_CERTS
    return;
#else
    if (!compiled_hash_known || compiled_hash_stored) return;
    if (!modem.mqtt_certs_written()) return;
    if (!preferences.begin("mqttcert", false)) return;
    preferences.putUInt("hash", compiled_cert_hash);
    preferences.end();
    compiled_hash_stored = true;
#endif
}

static void record_tls_code(int code) {
    if (!cert_tls_error(code)) return;
    char token[16];
    if (format_mqtt_net_err(code, token, sizeof(token)) < 0) return;
    daily_log_net_reason(token);
}

static bool cellular_tls_failure() {
    return comm.isCellular() && cert_tls_error(modem.mqtt_connect_code());
}

static bool mqtt_connect_once() {
    bool ok = mqtt_connect();
    if (comm.isCellular()) {
        record_tls_code(modem.mqtt_connect_code());
        remember_cert_hash();
    }
    return ok;
}

static void reset_mqtt_session() {
    comm.mqtt_disconnect(client_index);
    delay(1000);
    comm.mqtt_end();
    delay(1000);
    comm.mqtt_begin(true);
    comm.mqtt_set_certificate(AmazonRootCA, AWSClientCertificate, AWSClientPrivateKey);
}

static bool tls_recover_once() {
    if (tls_reload_used) return false;
    tls_reload_used = true;
#ifdef ALLOW_PLACEHOLDER_CERTS
    return false;
#else
    if (!comm.isCellular()) return false;
    feed_watchdog();
    modem.mqtt_force_cert_reload(true);
    reset_mqtt_session();
    return mqtt_connect_once();
#endif
}

// Initialize MQTT Connection
bool init_mqtt_connection() {
    bool mqtt_connect_result = false;
    unsigned long phase_start = millis();
    local_logger("MQTT Begin . . .");
    feed_watchdog();
    // Initialize MQTT, use SSL
    comm.mqtt_begin(true);

    local_logger("MQTT Set Certificates . . .");
    if (comm.isCellular()) arm_cert_upload();
    // Set Amazon Certificate
    comm.mqtt_set_certificate(AmazonRootCA, AWSClientCertificate, AWSClientPrivateKey);

    local_logger("MQTT Connecting . . .");
    // Connecting to AWS IOT Core
    mqtt_connect_result = mqtt_connect_once();
    // 32, 33, and 34 cannot succeed by repeating the same handshake.
    // Stop the 11-try loop, replace the modem files once, and connect once more.
    if (!mqtt_connect_result && cellular_tls_failure()) {
        mqtt_connect_result = tls_recover_once();
    }

    int k = 10;
    while (!mqtt_connect_result && (k > 0) && !tls_reload_used && !cellular_tls_failure()
        && !(comm.isCellular() && modem.mqtt_pem_refused())) {
        if (phase_deadline_hit(phase_start, MQTT_PHASE_BUDGET_MS)) {
            local_logger("init_mqtt_connection deadline");
            return false;
        }
        k--;
        // Check the connection every ten seconds
        #ifdef DEBUG
            Serial.println("Retrying MQTT Connect . . .");
        #endif
        mqtt_connect_result = mqtt_connect_once();
        if (!mqtt_connect_result && cellular_tls_failure()) {
            mqtt_connect_result = tls_recover_once();
            break;
        }
        if (!mqtt_connect_result) {
            if (comm.isCellular() && modem.mqtt_pem_refused()) break;
            if (k == 5) {
                //Try to reset the connection
                reset_mqtt_session();
            } else {
                delay(2000);
            }
        }
    }
    if (!mqtt_connect_result) {
        local_logger("MQTT No Connected . . .");
    }
    return mqtt_connect_result;
}


// Update Running Time Function
void update_running_time() {
    unsigned long adjustment = millis() + (TIME_TO_SLEEP_SEC * 1000);
    firmware_running_time = firmware_running_time + adjustment;
    gps_running_time = gps_running_time + adjustment;
}


// Set MQTT Subscription
void set_mqtt_subscription() {
    // Set MQTT processing callback
    comm.mqtt_set_callback(get_mqtt_parameters);
    // Subscribe to topic
    String final_subscribe_topic = subscribe_topic_prefix + ESP.getEfuseMac();
    if (comm.mqtt_subscribe(client_index, final_subscribe_topic.c_str())) {
        local_logger("Subscribed to topic: ", final_subscribe_topic);
    } else {
        local_logger("MQTT Subscribe Failed!");
    }
}

// Wait for sensors to stabilize

// Restart Function
void perform_restart() {
    radio_skip_clear(&radio_skip_remaining);

    // Before restarting, we want to make sure we spillover any remaining data in the RTC to the SD card to avoid data loss, 
    // if the SD card is working properly. This is especially important in case of a panic reset (watchdog timeout) 
    // where we might have data in the RTC that hasn't been spilled over yet.
    spillover_rtc_to_sd();
    delay(500); // Give some time for the spillover to complete before restarting

    local_logger("Initiating remote restart...");
    
    // 1. Disconnect safely (optional but good for broker health)
    mqtt_disconnect();
    
    Serial485.end(); // End RS485 communication

    // 2. Flush logs
    Serial.flush();

    // 3. Disconnect GPRS
    modem.gprsDisconnect();

    //4. Disconnect Modem
    force_modem_disconnect(modem); 

    // 5. Wait a moment
    delay(1000);
    
    // 6. Reset
    ESP.restart();
}


// Automatic Rain Sensor Reset Logic

// MQTT Connect Function
bool mqtt_connect() {
    local_logger("Connecting to ", broker);

    String dynamic_client_id = String(client_id_prefix) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    bool ret = comm.mqtt_connect(client_index, broker, broker_port, dynamic_client_id.c_str());
    if (!ret) {
        local_logger("Failed!"); return false;
    }

    if (comm.mqtt_connected()) {
        local_logger("MQTT has connected!");
    } else {
        return false;
    }
    return true;
}

// MQTT Disconnect Function
void mqtt_disconnect() {
    local_logger("MQTT Disconnecting . . .");
    comm.mqtt_disconnect(client_index);
    delay(500);
}


void clamp_runtime_params() {
    TIME_TO_SLEEP_SEC = clamp_u32(TIME_TO_SLEEP_SEC, TTS_SEC_MIN, TTS_SEC_MAX);
    TIME_TO_READ_SENSOR = clamp_u32(TIME_TO_READ_SENSOR, TTR_SENSOR_MIN_MS, TTR_SENSOR_MAX_MS);
    READING_COUNT = clamp_u32(READING_COUNT, R_COUNT_MIN, R_COUNT_MAX);
    if (TIME_BETWEEN_READINGS > TB_READINGS_MAX_SEC) TIME_BETWEEN_READINGS = TB_READINGS_MAX_SEC;
    READING_RATE = clamp_u32(READING_RATE, R_RATE_MIN, R_RATE_MAX);
    if (CHECK_FIRMWARE_UPDATE < C_F_UPDATE_MIN_MS) CHECK_FIRMWARE_UPDATE = C_F_UPDATE_MIN_MS;
    if (CHECK_FIRMWARE_UPDATE > C_F_UPDATE_MAX_MS) CHECK_FIRMWARE_UPDATE = C_F_UPDATE_MAX_MS;
    if (local_lat2 < -90.0f || local_lat2 > 90.0f) local_lat2 = 0;
    if (local_lon2 < -180.0f || local_lon2 > 180.0f) local_lon2 = 0;
}

void save_device_params() {
    preferences.begin("devcfg", false);
    preferences.putUInt("phash", g_rtc.param_hash);
    preferences.putUInt("rsid", g_rtc.last_restart_id);
    preferences.putUInt("tts", TIME_TO_SLEEP_SEC);
    preferences.putUInt("ttr", TIME_TO_READ_SENSOR);
    preferences.putUInt("rcount", READING_COUNT);
    preferences.putUInt("tbr", TIME_BETWEEN_READINGS);
    preferences.putUInt("rrate", READING_RATE);
    preferences.putBool("ggps", get_gps_data);
    preferences.putUInt("cfw", (uint32_t)CHECK_FIRMWARE_UPDATE);
    preferences.putFloat("lat", local_lat2);
    preferences.putFloat("lon", local_lon2);
    preferences.end();
}

void load_device_params() {
    if (!preferences.begin("devcfg", true)) return;
    if (preferences.isKey("apn")) {
        configured_apn = preferences.getString("apn", "");
        configured_apn_set = true;
    }
    if (net_sticky_since == 0 && preferences.isKey("nstk")) {
        net_sticky_since = preferences.getUInt("nstk", 0);
    }
    if (preferences.isKey("loglvl")) {
        daily_log_set_level_code(preferences.getUChar("loglvl", LOG_INFO));
    }
    if (!preferences.isKey("phash")) {
        preferences.end();
        return;
    }
    TIME_TO_SLEEP_SEC = preferences.getUInt("tts", TIME_TO_SLEEP_SEC);
    TIME_TO_READ_SENSOR = preferences.getUInt("ttr", TIME_TO_READ_SENSOR);
    READING_COUNT = preferences.getUInt("rcount", READING_COUNT);
    TIME_BETWEEN_READINGS = preferences.getUInt("tbr", TIME_BETWEEN_READINGS);
    READING_RATE = preferences.getUInt("rrate", READING_RATE);
    get_gps_data = preferences.getBool("ggps", get_gps_data);
    CHECK_FIRMWARE_UPDATE = preferences.getUInt("cfw", (uint32_t)CHECK_FIRMWARE_UPDATE);
    local_lat2 = preferences.getFloat("lat", local_lat2);
    local_lon2 = preferences.getFloat("lon", local_lon2);
    g_rtc.param_hash = preferences.getUInt("phash", 0);
    g_rtc.last_restart_id = preferences.getUInt("rsid", 0);
    active_device_type = preferences.getInt("dtype", active_device_type);
    timezone_offset_sec = preferences.getInt("tz", (int)timezone_offset_sec);
    String country = preferences.getString("ctry", "CR");
    (void)country;
    for (int i = 0; i < 4; i++) {
        String key = "p" + String(i);
        String plmn = preferences.getString(key.c_str(), "");
        if (plmn.length() > 0 && plmn.length() < 8) {
            strncpy(plmn_table[i], plmn.c_str(), 7);
            plmn_table[i][7] = '\0';
        }
    }
    preferences.end();
    clamp_runtime_params();
    rtc_persist_commit();
}

// JSON true, or any non-zero number. as<bool>() is not enough: ArduinoJson 7
// returns false for the 1 the backend sends.
static bool command_flag_on(JsonVariant value) {
    if (value.isNull()) return false;
    const bool is_bool = value.is<bool>();
    const bool is_number = !is_bool && !value.is<const char*>() && !value.is<JsonObject>() && !value.is<JsonArray>();
    const bool bool_value = is_bool && value.as<bool>();
    const long number = is_number ? value.as<long>() : 0L;
    return flag_is_on(true, is_bool, bool_value, is_number, number);
}

// MQTT Callback to receive parameters
void get_mqtt_parameters(const char *topic, const uint8_t *payload, uint32_t len) {

    #ifdef DEBUG
        Serial.println();
        Serial.println("======MQTT CALLBACK======");
        Serial.println();
        Serial.print("Topic:");
        Serial.println(topic);
    #endif
    if (payload == nullptr || len == 0) return;

    JsonDocument parameters_payload;
    DeserializationError json_error = deserializeJson(parameters_payload, payload, len);
    if (json_error) {
        local_logger("MQTT parameter JSON rejected");
        return;
    }

    uint32_t hash = payload_hash32(payload, len);
    bool restart_requested = false;
    uint32_t restart_id = 0;
    if (command_flag_on(parameters_payload["RESTART"])) {
        if (!parameters_payload["RESTART_ID"].isNull()) {
            restart_id = parameters_payload["RESTART_ID"].as<uint32_t>();
        }
        if (restart_id == 0) restart_id = hash;
        if (restart_id != g_rtc.last_restart_id) restart_requested = true;
    }

    JsonVariant rr_value = parameters_payload["RR_SENSOR"];
    const bool rr_on = command_flag_on(rr_value);
    const bool pulse_rain = rain_pulse_due(rr_on, g_rtc.rr_prev != 0);

    // Retained messages (60 s) are delivered again on the next connect.
    // Skip them when the body and the restart id are unchanged and the rain
    // command is not a rising edge. A repeated 1 stays quiet; 0 then 1 pulses.
    if (hash == g_rtc.param_hash && !restart_requested && !pulse_rain) {
        parameters_payload.clear();
        return;
    }

    if (!parameters_payload["SEND_LOGS"].isNull()) {
        preferences.begin("devcfg", false);
        preferences.putBool("slogs", parameters_payload["SEND_LOGS"].as<bool>());
        preferences.end();
    }
    if (!parameters_payload["LOG_LEVEL"].isNull()) {
        String level = parameters_payload["LOG_LEVEL"].as<String>();
        daily_log_set_level_text(level.c_str());
    }
    if (!parameters_payload["TTS_SEC"].isNull()) {
        TIME_TO_SLEEP_SEC = clamp_i32_to_u32(parameters_payload["TTS_SEC"].as<long>(), TTS_SEC_MIN, TTS_SEC_MAX);
    }
    if (!parameters_payload["TTR_SENSOR"].isNull()) {
        TIME_TO_READ_SENSOR = clamp_i32_to_u32(parameters_payload["TTR_SENSOR"].as<long>(), TTR_SENSOR_MIN_MS, TTR_SENSOR_MAX_MS);
    }
    if (!parameters_payload["R_COUNT"].isNull()) {
        READING_COUNT = clamp_i32_to_u32(parameters_payload["R_COUNT"].as<long>(), R_COUNT_MIN, R_COUNT_MAX);
    }
    if (!parameters_payload["TB_READINGS"].isNull()) {
        long gap = parameters_payload["TB_READINGS"].as<long>();
        if (gap < 0) gap = 0;
        if (gap > (long)TB_READINGS_MAX_SEC) gap = TB_READINGS_MAX_SEC;
        TIME_BETWEEN_READINGS = (unsigned int)gap;
    }
    if (!parameters_payload["R_RATE"].isNull()) {
        READING_RATE = clamp_i32_to_u32(parameters_payload["R_RATE"].as<long>(), R_RATE_MIN, R_RATE_MAX);
    }
    if (!parameters_payload["G_GPS_DATA"].isNull()) {
        get_gps_data = command_flag_on(parameters_payload["G_GPS_DATA"]);
    }
    if (!parameters_payload["C_F_UPDATE"].isNull()) {
        long check_ms = parameters_payload["C_F_UPDATE"].as<long>();
        if (check_ms < (long)C_F_UPDATE_MIN_MS) check_ms = (long)C_F_UPDATE_MIN_MS;
        if (check_ms > (long)C_F_UPDATE_MAX_MS) check_ms = (long)C_F_UPDATE_MAX_MS;
        CHECK_FIRMWARE_UPDATE = (unsigned long)check_ms;
    }
    if (!parameters_payload["L_LAT"].isNull()) {
        float lat = parameters_payload["L_LAT"].as<float>();
        if (lat >= -90.0f && lat <= 90.0f) local_lat2 = lat;
    }
    if (!parameters_payload["L_LONG"].isNull()) {
        float lon = parameters_payload["L_LONG"].as<float>();
        if (lon >= -180.0f && lon <= 180.0f) local_lon2 = lon;
    }

    const uint8_t rr_next = rr_value.isNull() ? g_rtc.rr_prev : (uint8_t)(rr_on ? 1 : 0);
    const bool rr_changed = g_rtc.rr_prev != rr_next;
    g_rtc.rr_prev = rr_next;

    if (hash != g_rtc.param_hash || restart_requested || rr_changed) {
        g_rtc.param_hash = hash;
        if (restart_requested) g_rtc.last_restart_id = restart_id;
        save_device_params();
        rtc_persist_commit();
    }

    // Pulse while this callback is still awake. Sleep later holds GPIO46 low,
    // so the 5 s HIGH must finish before get_mqtt_parameters returns.
    if (pulse_rain) {
        local_logger("Reset rain sensor command received");
        reset_sensor();
    }

    parameters_payload.clear();

    if (restart_requested) {
        local_logger("Remote restart command received");
        delay(500);
        perform_restart();
    }
}

// Check Firmware Update Function
JsonDocument check_firmware_update(){

    JsonDocument parameters_payload;

    // Initialize HTTPS
    comm.https_begin();

    // Set GET URT
    if (!comm.https_set_url(CHECK_FIRMWARE_UPDATE_URL)) {
        Serial.println("Failed to set the URL. Please check the validity of the URL!");
        comm.https_end();
        return parameters_payload;
    } 
    // Send GET request
    comm.https_add_header("X-API-Key", CHECK_FIRMWARE_KEY);
    int httpCode = 0;
    char post_payload[256];
    int post_len = firmware_update_request_body(
        post_payload,
        sizeof(post_payload),
        client_id_prefix,
        active_device_type,
        VARIANT_ID,
        firmware_version,
        ESP.getEfuseMac());
    if (post_len < 0) {
        Serial.println("Firmware update request body did not fit");
        comm.https_end();
        return parameters_payload;
    }
    httpCode = comm.https_post(post_payload);
    if (httpCode != 200) {
        Serial.print("HTTP POST failed ! error code = "); 
        Serial.println(httpCode); 
        comm.https_end();
        local_logger("HTTP get failed CHECK FIRMWARE UPDATE");
        return parameters_payload;
    }
    String body = comm.https_body();
    #ifdef DEBUG
        Serial.print("HTTP body : ");
        Serial.println(body);
    #endif

    //Conver the String to JSON Document
    deserializeJson(parameters_payload, body);

    comm.https_end();

    return parameters_payload;
}


// S3_CONFIG.SHA256 is lowercase hex of the image. S3_CONFIG.SIGNATURE is base64 DER ECDSA P-256.
// Unsigned images are rejected. The signature is checked before Update.end().
void firmware_update(const char *bucket_name, const char *file_name, const char* sha256_hex, const char* signature_b64){
    if (!sha256_hex || !sha256_hex[0] || !signature_b64 || !signature_b64[0]) {
        local_logger("OTA rejected. S3_CONFIG.SHA256 (hex) and S3_CONFIG.SIGNATURE (base64) are required. Unsigned images are not installed.");
        add_sensor_error("[OTA_UNSIGNED]");
        return;
    }

    // Initialize HTTPS
    comm.https_begin();
    local_logger("Getting firmware file. . .");

    String final_url = String("https://alxv44fafe.execute-api.us-east-1.amazonaws.com/") + API_STAGE + "/";
    final_url.concat(bucket_name);
    final_url.concat("/");
    final_url.concat(file_name);
    local_logger("File Update URL: ");
    local_logger(final_url);
    if (!comm.https_set_url(final_url)) {
        Serial.println("Failed to set the URL. Please check the validity of the URL!");
        comm.https_end();
        local_logger("Failed to set the URL. Please check the validity of the URL!");
        return;
    } 

    //Set API Key
    comm.https_add_header("X-API-Key", FIRMWARE_UPDATE_KEY);
    int httpCode = 0;
    httpCode = comm.https_get();
    if (httpCode != 200 && strcmp(API_STAGE, "dev") != 0) {
        comm.https_end();
        comm.https_begin();
        final_url = String("https://alxv44fafe.execute-api.us-east-1.amazonaws.com/dev/");
        final_url.concat(bucket_name);
        final_url.concat("/");
        final_url.concat(file_name);
        if (comm.https_set_url(final_url)) {
            comm.https_add_header("X-API-Key", FIRMWARE_UPDATE_KEY);
            httpCode = comm.https_get();
        }
    }
    if (httpCode != 200) {
        Serial.print("HTTP GET failed ! error code = "); 
        Serial.println(httpCode); 
        local_logger("HTTP GET failed ! FIRMWARE UPDATE");
        comm.https_end();
        return;
    }
    local_logger("Reading firmware file. . .");
    feed_watchdog();
    unsigned long size_wait_start = millis();
    size_t firmware_size = comm.https_get_size();
    feed_watchdog();
    if (firmware_size == 0 || (millis() - size_wait_start) > HTTPS_SIZE_BUDGET_MS) {
        local_logger("https_get_size failed or passed its deadline");
        if (firmware_size == 0) {
            comm.https_end();
            return;
        }
    }
    
    // Begin Update firmware
    Serial.println("Start upgrade firmware ...");
    if (!Update.begin(firmware_size)) {
        Serial.println("Not enough space to begin OTA");
        comm.https_end();
        return;
    }

    uint8_t buffer[1024];
    int written = 0;
    int progress = 0;
    int total = 0;
    bool sha_ok = false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    sha_ok = mbedtls_sha256_starts_ret(&sha, 0) == 0;

    uint32_t update_timeout_start = millis();    

    while (total < firmware_size) {
        // Read firmware from modem buffer
        int len = comm.https_body(buffer, 1024);
        
        if (len > 0) {
            written = Update.write(buffer, len);
            if (written != len) {
                mbedtls_sha256_free(&sha);
                Update.abort();
                comm.https_end();
                return;
            }
            if (!sha_ok || mbedtls_sha256_update_ret(&sha, buffer, (size_t)written) != 0) sha_ok = false;
            total += written;
            int newProgress = (total * 100) / firmware_size;
            
            if (newProgress - progress >= 5 || newProgress == 100) {
                progress = newProgress;
                Serial.print(String("\r ") + progress + "%\n");
                feed_watchdog(); // Crucial: Keep WDT alive during long cellular downloads
            }
            update_timeout_start = millis(); // Reset timeout on successful read
        } else {
            // Buffer is temporarily empty, wait for the next chunk
            delay(100);
            feed_watchdog(); 
            
            if (millis() - update_timeout_start > OTA_TIMEOUT) {
                Serial.println("\n[ERROR] Network timeout waiting for firmware chunks.");
                break; // Break to safely abort and restart device
            }
        }
    }

    uint8_t image_hash[32];
    memset(image_hash, 0, sizeof(image_hash));
    if (sha_ok) sha_ok = mbedtls_sha256_finish_ret(&sha, image_hash) == 0;
    mbedtls_sha256_free(&sha);

    if ((size_t)total != firmware_size || !sha_ok) {
        local_logger("OTA rejected. Download was incomplete or the SHA-256 could not be computed.");
        Update.abort();
        comm.https_end();
        return;
    }
    char actual_hex[65];
    sha256_to_hex(image_hash, actual_hex);
    OtaPrecheck offer = ota_precheck(sha256_hex, signature_b64, actual_hex);
    if (offer != OTA_PRECHECK_NEED_SIGNATURE) {
        if (offer == OTA_PRECHECK_REJECT_MISSING) {
            local_logger("OTA rejected. SHA256 or SIGNATURE is missing. Unsigned images are not installed.");
            add_sensor_error("[OTA_UNSIGNED]");
        } else {
            local_logger("OTA rejected. SHA256 does not match the downloaded image.");
            add_sensor_error("[OTA_BAD_HASH]");
        }
        Update.abort();
        comm.https_end();
        return;
    }
    if (!ota_signature_valid(image_hash, signature_b64)) {
        add_sensor_error("[OTA_BAD_SIGNATURE]");
        Update.abort();
        comm.https_end();
        return;
    }
    ota_remember_slot();
    if (!Update.end()) {
        Serial.println("Update not finished? Something went wrong!");
        Serial.println("Error Occurred. Error #: " + String(Update.getError()));
        comm.https_end();
        return;
    }

    Serial.println();

    if (Update.isFinished()) {
        Serial.println("Update successfully completed.");
    }

    Serial.println("=== Update successfully completed. Rebooting.");

    delay(500);

    comm.https_end();

    esp_restart();    
    
}

// Check for Firmware Update Function
void check_for_firmware_update(bool force_check) {
    if (awake_budget_expired()) {
        local_logger("Awake budget used. Skipping firmware check.");
        return;
    }

    local_logger("Checking for an Update!");    
    #ifdef DEBUG
        Serial.println("Time Now: ");
        Serial.println(millis());
        Serial.println("Running Time Now: ");
        Serial.println(firmware_running_time);
        Serial.println("CHECK_FIRMWARE_UPDATE: ");
        Serial.println(CHECK_FIRMWARE_UPDATE);
    #endif
    if (firmware_running_time >= CHECK_FIRMWARE_UPDATE || force_check) {
        //Time to check for a firmware update
        local_logger("Time to check for an Update!");
        firmware_running_time = 0;

        JsonDocument update_parameters = check_firmware_update();

        String str_param_1 = update_parameters["UPDATE_AVAILABLE"];
        bool new_firmware_available = str_param_1.toInt();
        String bucket_name = update_parameters["BUCKET_NAME"];
        String file_name = update_parameters["FILE_NAME"];
        String offered_variant = update_parameters["VARIANT"] | update_parameters["S3_CONFIG"]["VARIANT"] | "";
        int offered_version = update_parameters["FIRMWARE_VERSION"] | update_parameters["S3_CONFIG"]["VERSION"] | 0;
        if (offered_variant.length() > 0 && offered_variant != VARIANT_ID) {
            local_logger("OTA rejected. Variant does not match this build.");
            return;
        }
        if (offered_version > 0 && offered_version <= firmware_version) {
            local_logger("OTA rejected. Version is not newer.");
            return;
        }

        if (new_firmware_available) {
            String offered_sha = update_parameters["S3_CONFIG"]["SHA256"] | "";
            String offered_sig = update_parameters["S3_CONFIG"]["SIGNATURE"] | "";
            offered_sha.trim();
            offered_sig.trim();
            if (offered_sha.length() == 0 || offered_sig.length() == 0) {
                local_logger("OTA rejected. S3_CONFIG.SHA256 (hex) and S3_CONFIG.SIGNATURE (base64) are required. Unsigned images are not installed.");
                add_sensor_error("[OTA_UNSIGNED]");
                return;
            }
            // Save the messages in RTC memory before
            spillover_rtc_to_sd();
            delay(500); // Give some time for the spillover to complete before starting the update process

            local_logger("New Firmware Available!");
            local_logger("Trying to Update . . .");
            local_logger(bucket_name);
            local_logger(file_name);
            firmware_update(bucket_name.c_str(), file_name.c_str(), offered_sha.c_str(), offered_sig.c_str());
        } else {
            local_logger("NO New Firmware!");
        }
    }
}

void power_off_modem_for_sleep();

// Go to Deep Sleep Function
void go_sleep(){
    #ifdef DEBUG
        Serial.println("Going to Sleep . . .");
    #endif

    try {
        //After receiving the parameters disconnect the MQTT client
        mqtt_disconnect();

        // End RS485
        Serial485.end();

        //Disable GPS
        modem.disableGPS();

        //Disconnect 
        force_modem_disconnect(modem); 
        // Give some time for the disconnect to complete before cutting power to the modem
        delay(1000);

        modem.disableNetwork(); 

        SD.end();
        delay(200);
        SPI.end(); 

    }
    catch(const std::exception& e) {
        local_logger(e.what());
    }

    if (!modem_was_powered) {
        local_logger("Modem was not powered. Skipping CPOF.");
    } else {
    #ifdef DEBUG
        Serial.println("Enter modem power off!");
    #endif
    power_off_modem_for_sleep();
    }

    pinMode(PIN_5V_EN, OUTPUT);
    digitalWrite(PIN_5V_EN, LOW);
    gpio_hold_en((gpio_num_t)PIN_5V_EN);
    if (rtc_gpio_is_valid_gpio((gpio_num_t)PIN_5V_EN)) {
        rtc_gpio_hold_en((gpio_num_t)PIN_5V_EN);
    }

    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, LOW);
    gpio_hold_en((gpio_num_t)RS485_EN_PIN);

    pinMode(RESET_RAIN_SENSOR_PIN, OUTPUT);
    digitalWrite(RESET_RAIN_SENSOR_PIN, LOW);
    gpio_hold_en((gpio_num_t)RESET_RAIN_SENSOR_PIN);

    pinMode(BOARD_SD_CS_PIN, OUTPUT);
    digitalWrite(BOARD_SD_CS_PIN, HIGH);
    gpio_hold_en((gpio_num_t)BOARD_SD_CS_PIN);

    ledcDetachPin(LED_RED_PIN);
    ledcDetachPin(LED_GREEN_PIN);
    ledcDetachPin(LED_BLUE_PIN);

    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);

    #ifdef MODEM_RESET_PIN
        digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
        gpio_hold_en((gpio_num_t)MODEM_RESET_PIN);
    #endif
    // GPIO 15 floats during deep sleep unless held. A float presses the
    // key for the whole 20s floor and the module is already on at wake.
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    gpio_hold_en((gpio_num_t)BOARD_PWRKEY_PIN);
    gpio_deep_sleep_hold_en();

    #ifdef DEBUG
        Serial.println("Weather Station is going to  deep sleep!");
    #endif

    uint32_t awake_sec = (millis() - g_wake_start_ms) / 1000UL;
    uint32_t sleep_sec = sleep_sec_after_wake(awake_sec, active_sleep_sec);
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * uS_TO_S_FACTOR);
    delay(200);

    esp_deep_sleep_start();
    Serial.println("#### This will never be printed ####");
}

// Wakeup Function

// Initialize Network

// Get Sensor Payload
// NUEVO: Se agregó el parámetro 'String value_types[]'

// Get Sensor Configuration from Server


// Clear Sensors Configuration from Preferences


// Prepare Async Sensors: Trigger and Wait for Ready

// Get Final Sensor Payload

void attach_daily_health(JsonDocument& doc);

// Get Final Payload
String get_final_payload(String sensors_payload, String gps_payload, bool result) {

    JsonDocument doc;
    String final_payload = "";
    String sensor_payload_errors = "";
    size_t final_payload_size;
    unsigned long final_timestamp = get_current_timestamp();

    if (sensors_payload == "" || sensors_payload == "null" || sensors_payload == "{}") {
        consecutive_null_errors++;
        rtc_persist_commit();
        local_logger("Critical error: null sensor payload. Queueing an error record.");
        add_sensor_error("[NULL_PAYLOAD_ERROR]");
        sensors_payload = "{\"err\":\"NULL_PAYLOAD\",\"n\":" + String((int)consecutive_null_errors) + "}";
    } else if (consecutive_null_errors > 0) {
        local_logger("Sensors recovered. Cleared the RTC failure counter.");
        consecutive_null_errors = 0;
        rtc_persist_commit();
    }

    doc["sp"] = serialized(sensors_payload);
    doc["gps_p"] = serialized(gps_payload);

    doc["fv"] = firmware_version;
    doc["uid"] = String(ESP.getEfuseMac());
    doc["dt"] = String(active_device_type);
    doc["net"] = registered_plmn;
    doc["sim"] = sim_label;
    doc["ts"] = (int)current_time_source;
    doc["tm"] = final_timestamp;
    doc["eff_sleep"] = active_sleep_sec;
    doc["power_mode"] = active_power_mode;
    doc["next_msg_at"] = (long)final_timestamp + (long)active_sleep_sec;
    if (announce_power_mode) {
        doc["evt"] = "MODE_CHANGE";
        doc["pack_v"] = last_pack_volts;
    }

    if (!is_sdcard_ok) {
        doc["sd_st"] = false;
    }

    // SD Card Errors: Add any SD card errors to the final payload and clear the error log
    String sd_errors = fetch_and_clear_sd_errors();
    if (sd_errors.length() > 0) {
        sd_errors.trim(); // Limpiamos espacios al inicio o final
        doc["sd_err"] = sd_errors; // Se agrega la llave solo si hay errores
    }

    // Sensor Errors: Add any sensor errors to the final payload and clear the error log
    String sensor_errors = fetch_and_clear_sensor_errors();
    if (sensor_errors.length() > 0) {
        sensor_errors.trim(); // Limpiamos espacios al inicio o final
        doc["sensor_err"] = sensor_errors; // Se agrega la llave solo si hay errores
    }
    char reset_errors[RTC_ERR_SLOTS * RTC_ERR_BYTES];
    if (rtc_error_ring_format(reset_errors, sizeof(reset_errors)) > 0) {
        doc["rst_err"] = reset_errors;
        rtc_error_ring_clear();
    }
    attach_daily_health(doc);

    serializeJson(doc, final_payload);

    local_logger("FINAL PAYLOAD GENERATED: " + final_payload);

    return final_payload;
}

// Get GPS Payload

// Get GPS Payload with Cache


/**
 * Try to send all queued data (RTC y SD)
 * processing by batch size MAX_BATCH_SIZE.
 */
bool send_all_queued_data() {
    if (!has_pending_data()) {
        local_logger("No queued data to send.");
        return true; 
    }

    local_logger("Connecting so the queue can drain...");
    if (!init_mqtt_connection()) {
        local_logger("MQTT connect failed. Data kept locally.");
        return false;
    }
    
    set_mqtt_subscription();
    bool all_batches_ok = true;
    int batches_sent = 0;

    // Bucle de paginación: mientras existan registros en RTC o SD...
    while (has_pending_data() && batches_sent < max_backlog_batches) {
        feed_watchdog(); // Alimentamos el WDT en cada lote por seguridad

        // Extrae un arreglo JSON de hasta MAX_BATCH_SIZE lecturas: "[{...},{...}]"
        String batch_payload = get_batch_payload(); 
        if (batch_payload == "") break;

        local_logger("Publishing a queued batch...");
        if (publish_sensor_payload(batch_payload)) {
            // Éxito: Se eliminan definitivamente estos registros de la fuente (RTC o SD)
            commit_batch();
            batches_sent++; 
            local_logger("Batch published and committed.");
            
            // Un breve respiro para procesar respuestas del broker si es necesario
            comm.mqtt_handle(5000); 
        } else {
            // Fallo de red/comunicación durante el envío
            local_logger("Batch publish failed. Rolling back...");
            rollback_batch(); // Si venía de RTC y la SD está lista, los "derrama" a la SD
            all_batches_ok = false;
            break; // Abortamos el bucle para no seguir perdiendo paquetes
        }
    }

    return all_batches_ok;
}

#include "wake_cycle.h"
#include "network.h"
#include "sensors.h"
#include "gps_module.h"
#include "power_policy.h"

void setup() {
    WakeState wake;
    boot_start(wake);
    network_provision(wake);
    wake.sensors = sensors_sample(&wake.configured);
    apply_pack_power(wake.sensors);
    turn_sensors_off();
    wake.gps = gps_sample(modem_was_powered, first_run);
    network_transmit_if_needed(wake);
    deliver_sample(wake);
    sleep_now();
}

void loop() {
}


static bool json_number(JsonVariant value) {
    return !value.isNull() && !value.is<bool>() && !value.is<const char*>() && !value.is<JsonObject>() && !value.is<JsonArray>();
}

// Sample payload is {"v_s_p_0":{"b_v_v":1199}}. A full publish document nests that map under "sp".
static bool pack_raw_from_payload(const String& payload, float* raw) {
    JsonDocument doc;
    if (deserializeJson(doc, payload)) return false;
    JsonObject sensors = doc.as<JsonObject>();
    if (sensors.isNull()) return false;
    JsonVariant wrapped = doc["sp"];
    if (wrapped.is<JsonObject>()) sensors = wrapped.as<JsonObject>();
    for (JsonPair entry : sensors) {
        JsonObject reading = entry.value().as<JsonObject>();
        if (reading.isNull()) continue;
        if (!reading["err"].isUnbound()) continue;
        JsonVariant value = reading[PACK_V_VALUE_NAME];
        if (!json_number(value)) continue;
        *raw = value.as<float>();
        return true;
    }
    return false;
}

void apply_pack_power(const String& payload) {
    float scale = PACK_V_SCALE;
    float backend_scale = 0.0f;
    bool backend_scale_found = sensor_measure_scale_find(PACK_V_VALUE_NAME, &backend_scale);
    if (backend_scale_found) scale = backend_scale;

    float raw = 0.0f;
    bool saw_raw = pack_raw_from_payload(payload, &raw);
    PackVolts pv;
    pv.volts = -1.0f;
    pv.valid = false;
    if (saw_raw) pv = pack_volts_from_raw(raw, scale);

    #ifdef DEBUG
        Serial.print("Pack voltage ");
        if (saw_raw) {
            Serial.print(pv.volts, 2);
            Serial.print(" V");
        } else {
            Serial.print("absent");
        }
        Serial.print(" scale source ");
        Serial.println(backend_scale_found ? "backend" : "build");
    #endif

    PowerInput in = {};
    in.pack_volts = pv.volts;
    in.pack_valid = pv.valid;
    in.previous = (PowerMode)saved_power_mode;
    in.backend_sleep_sec = TIME_TO_SLEEP_SEC;
    in.backend_reading_count = READING_COUNT;
    PowerDecision decision = decide_power(in);
    active_power_mode = (uint8_t)decision.mode;
    active_sleep_sec = decision.eff_sleep_sec;
    announce_power_mode = decision.changed;
    saved_power_mode = decision.mode;
    last_pack_volts = pv.valid ? pv.volts : -1;
    if (pv.valid) daily_log_note_voltage(pv.volts);
    if (!pv.valid) {
        local_logger("Pack voltage missing or failed. Keeping the backend interval.");
        add_sensor_error("[PACK_V_MISSING]");
    }
}


void maybe_upload_logs() {
    if (active_power_mode != POWER_NORMAL) return;
    bool send_logs = false;
    preferences.begin("devcfg", true);
    send_logs = preferences.getBool("slogs", false);
    preferences.end();
    if (!send_logs && !announce_power_mode) return;
    local_logger("Log upload is armed. The backend must supply a pre-signed URL.");
}

