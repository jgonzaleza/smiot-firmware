/**
 * Smart IoT End Node Firmware v200
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
#include "power_policy.h"
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

// Network
#define DEFAULT_NETWORK_APN "hologram" // Default APN
const char *hologram_apn = "hologram"; 
const char *hologram_id = "89464";
const char *emnify_apn = "emnify";
const char *emnify_id = "89883";
String global_apn = DEFAULT_NETWORK_APN;
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
const char *CHECK_FIRMWARE_UPDATE_URL = "https://junkzqt2yc.execute-api.us-east-1.amazonaws.com/pro/device_firmware_config";
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
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
RTC_DATA_ATTR uint8_t radio_skip_remaining = 0;
RTC_DATA_ATTR uint8_t gps_fail_streak = 0;
RTC_DATA_ATTR int saved_power_mode = POWER_NORMAL;
float last_pack_volts = -1;

// Global Flags
bool reset_rain_sensor = false; //Flag to reset rain sensor

//We only support 8 configuration for sensors
const int namespaces_size = 8;
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

// Initialize MQTT Connection
bool init_mqtt_connection() {
    bool mqtt_connect_result = false;
    unsigned long phase_start = millis();
    local_logger("MQTT Begin . . .");
    feed_watchdog();
    // Initialize MQTT, use SSL
    comm.mqtt_begin(true);

    local_logger("MQTT Set Certificates . . .");
    // Set Amazon Certificate
    comm.mqtt_set_certificate(AmazonRootCA, AWSClientCertificate, AWSClientPrivateKey);
    
    local_logger("MQTT Connecting . . .");
    // Connecting to AWS IOT Core
    mqtt_connect_result = mqtt_connect();

    int k = 10;
    while (!mqtt_connect_result && (k > 0)) {
        if (phase_deadline_hit(phase_start, MQTT_PHASE_BUDGET_MS)) {
            local_logger("init_mqtt_connection deadline");
            return false;
        }
        k--;
        // Check the connection every ten seconds
        #ifdef DEBUG
            Serial.println("Retrying MQTT Connect . . .");
        #endif        
        mqtt_connect_result = mqtt_connect();
        if (!mqtt_connect_result) {            
            if (k == 5){
                //Try to reset the connection
                comm.mqtt_disconnect(client_index);
                delay(1000);
                comm.mqtt_end();
                delay(1000);
                comm.mqtt_begin(true);
                comm.mqtt_set_certificate(AmazonRootCA, AWSClientCertificate, AWSClientPrivateKey);
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

// Restart Function
void perform_restart() {

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
    if (!parameters_payload["RESTART"].isNull() && parameters_payload["RESTART"].as<bool>()) {
        if (!parameters_payload["RESTART_ID"].isNull()) {
            restart_id = parameters_payload["RESTART_ID"].as<uint32_t>();
        }
        if (restart_id == 0) restart_id = hash;
        if (restart_id != g_rtc.last_restart_id) restart_requested = true;
    }

    // Retained messages (60 s) are delivered again on the next connect.
    // Skip them when neither the body nor the restart id is new.
    if (hash == g_rtc.param_hash && !restart_requested) {
        parameters_payload.clear();
        return;
    }

    if (!parameters_payload["RR_SENSOR"].isNull() && parameters_payload["RR_SENSOR"].as<bool>()) {
        local_logger("Reset rain sensor command received");
        reset_sensor();
    }

    if (!parameters_payload["SEND_LOGS"].isNull()) {
        preferences.begin("devcfg", false);
        preferences.putBool("slogs", parameters_payload["SEND_LOGS"].as<bool>());
        preferences.end();
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
        get_gps_data = parameters_payload["G_GPS_DATA"].as<bool>();
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

    if (hash != g_rtc.param_hash || restart_requested) {
        g_rtc.param_hash = hash;
        if (restart_requested) g_rtc.last_restart_id = restart_id;
        save_device_params();
        rtc_persist_commit();
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
    String post_payload = "{\"client_id\": \"";
    post_payload.concat(client_id_prefix);
    post_payload.concat("\", \"device_type\":");
    post_payload.concat(active_device_type);
    post_payload.concat(", \"variant\":\"");
    post_payload.concat(VARIANT_ID);
    post_payload.concat(", \"firmware_version\":");
    post_payload.concat(firmware_version);
    post_payload.concat(", \"device_id\": \"");
    post_payload.concat(ESP.getEfuseMac());
    post_payload.concat("\"}");
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


// Firmware Update Function
void firmware_update(const char *bucket_name, const char *file_name){

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
        return;
    }

    uint8_t buffer[1024];
    int written = 0;
    int progress = 0;
    int total = 0;

    uint32_t update_timeout_start = millis();    

    while (total < firmware_size) {
        // Read firmware from modem buffer
        int len = comm.https_body(buffer, 1024);
        
        if (len > 0) {
            written = Update.write(buffer, len);
            if (written != len) {
                Update.abort();
                comm.https_end();
                return;
            }
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

    if ((size_t)total != firmware_size) {
        Update.abort();
        comm.https_end();
        return;
    }
    ota_remember_slot();
    if (!Update.end()) {
        Serial.println("Update not finished? Something went wrong!");
        Serial.println("Error Occurred. Error #: " + String(Update.getError()));
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
            // Save the messages in RTC memory before
            spillover_rtc_to_sd();
            delay(500); // Give some time for the spillover to complete before starting the update process

            local_logger("New Firmware Available!");
            local_logger("Trying to Update . . .");
            local_logger(bucket_name);
            local_logger(file_name);
            firmware_update(bucket_name.c_str(), file_name.c_str());
        } else {
            local_logger("NO New Firmware!");
        }
    }
}

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
    feed_watchdog();
    modem.poweroff();
    delay(5000);
    feed_watchdog();
    int k = 10;
    while (modem.testAT(500) && (k > 0)) {
        delay(500);
        k--;
        feed_watchdog();
    }
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
    gpio_deep_sleep_hold_en();

    #ifdef DEBUG
        Serial.println("Weather Station is going to  deep sleep!");
    #endif

    uint32_t awake_sec = (millis() - g_wake_start_ms) / 1000UL;
    uint32_t sleep_sec = active_sleep_sec;
    if (awake_sec + 1 < sleep_sec) sleep_sec = sleep_sec - awake_sec;
    else sleep_sec = 1;
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * uS_TO_S_FACTOR);
    delay(200);

    esp_deep_sleep_start();
    Serial.println("#### This will never be printed ####");
}

// Wakeup Function
void init_modem() {
    Serial.begin(115200); // Set console baud rate

    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        #ifdef DEBUG
            Serial.println("Wakeup timer");
            Serial.println("Turn ON Modem!");
        #endif
    }

    gpio_hold_dis((gpio_num_t)PIN_5V_EN);
    gpio_hold_dis((gpio_num_t)RS485_EN_PIN);
    gpio_hold_dis((gpio_num_t)RESET_RAIN_SENSOR_PIN);
    gpio_hold_dis((gpio_num_t)BOARD_SD_CS_PIN);
    gpio_deep_sleep_hold_dis();

    #ifdef MODEM_RESET_PIN
        gpio_hold_dis((gpio_num_t)MODEM_RESET_PIN);
        pinMode(MODEM_RESET_PIN, OUTPUT);
        digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL); delay(100);
        digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL); delay(2600);
        digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
    #endif

    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(1000);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);

    // Delay sometime ...
    delay(1000);
    #ifdef DEBUG
        Serial.println("Checking if modem is online.");
    #endif
    int retry = 0;
    int k = 50;
    while (!modem.testAT(1000) && k > 0) {
        k--;
        if (retry++ > 10) {
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            delay(100);
            digitalWrite(BOARD_PWRKEY_PIN, HIGH);
            delay(1000);
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            retry = 0;
        }
    }
    modem_was_powered = true;

    //Put the Led Pin as Output
    pinMode(LED_RED_PIN, OUTPUT);
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_BLUE_PIN, OUTPUT);

    pinMode(RESET_RAIN_SENSOR_PIN, OUTPUT);
    digitalWrite(RESET_RAIN_SENSOR_PIN, LOW);   

    
    #ifdef DEBUG
        Serial.println("Modem is online!");
        Serial.println("#### Wakeup Completed ####");
    #endif
}

// Initialize Network
bool init_network(NetworkMode mode_setup_value, bool show_status=false) {
    unsigned long phase_start = millis();
    // Check if SIM card is online
    SimStatus sim = SIM_ERROR;
    bool result = false;
    int k = 30;
    while ((sim != SIM_READY) && (k > 0)) {
        if (phase_deadline_hit(phase_start, NET_PHASE_BUDGET_MS)) {
            local_logger("init_network deadline");
            return false;
        }
        k--;
        sim = modem.getSimStatus();
        switch (sim) {
            case SIM_READY:
                #ifdef DEBUG
                    Serial.println("SIM card online");
                #endif
                break;
            case SIM_LOCKED:
                Serial.println("The SIM card is locked. Please unlock the SIM card first.");
                break;
            default:
                break;
        }        
        
        if (show_status) {
            //The blink gives us an one second delay
            blink_led(0, 255, 0);
        } else {
            delay(1000);
        }        
    }

    if (sim != SIM_READY) {
        if (show_status) {
            blink_led(255, 0, 0, 10000);
        }
        return result;
    }

    if (!modem.setNetworkMode(mode_setup_value)) {
        local_logger("Set network mode failed!");
    }
   
    String at_response;
    //Set APN for Global SIMs
    #ifdef DEFAULT_NETWORK_APN
        local_logger("Detecting SIM type via ICCID...");
        
        String iccid = modem.getSimCCID();
        bool requires_apn = false; 
        
        if (iccid.length() > 10) { 
            local_logger("ICCID: ", iccid);
            
            if (iccid.startsWith("89464")) {
                global_apn = String(hologram_apn); 
                requires_apn = true;
                local_logger("SIM Detected: Hologram");
            } 
            else if (iccid.startsWith("89883")) {
                global_apn = String(emnify_apn);
                requires_apn = true;
                local_logger("SIM Detected: Emnify");
            } 
            else {
                global_apn = "local_carrier"; // Mark it for the backend
                requires_apn = false;
                local_logger("SIM Detected: Local Carrier");
            }

            // Only set the PDP Context if the SIM explicitly needs it
            if (requires_apn) {
                local_logger("Setting network APN to: ", global_apn);
                modem.sendAT(GF("+CGDCONT=1,\"IP\",\""), global_apn, "\"");
                
                if (modem.waitResponse() != 1) {
                    local_logger("Error: Failed to set APN context!");
                    if (show_status) {
                        blink_led(255, 0, 0, 10000);
                    }                
                }            
            } else {
                local_logger("Skipping manual APN configuration.");
            }
            
        } else {
            local_logger("Error: Failed to read ICCID or SIM missing.");
            if (show_status) {
                blink_led(255, 0, 0, 10000);
            }
        } 
    #endif

    // ==========================================================
    // NUEVA LÓGICA DE ROTACIÓN DE RED (ANTI-ZOMBIE)
    // ==========================================================
    if (consecutive_network_changes >= MAX_NETWORK_FAILURES) {
        current_network_index++; // Try next network option
        if (current_network_index > 3) {
            current_network_index = 0; // Back to automatic mode after trying all options
        }
        // Reset the counter after a network change to give it a fresh start
        consecutive_network_changes = 0; 
        local_logger("Fallos maximos alcanzados. Cambiando red a indice: ", current_network_index);
    }

    if (current_network_index == 0 || global_apn == "local_carrier") {
        local_logger("Red en modo Automatico (+COPS=0)");
        modem.sendAT(GF("+COPS=0")); // Automatic mode
        modem.waitResponse(10000L, at_response);
        local_logger("Registro automático: ", at_response);
    } else {
        String plmn = plmn_table[current_network_index];
        if (plmn.length() == 0) plmn = "AUTO";
        local_logger("Forzando red manual (+COPS=1,2): ", plmn);
        
        // A. Forzamos el desregistro primero (Limpiar caché de la red zombie)
        modem.sendAT(GF("+COPS=2"));
        modem.waitResponse(5000L, at_response);
        local_logger("Desregistro forzado: ", at_response);
        
        // B. Intentamos el registro manual usando el código PLMN
        modem.sendAT(GF("+COPS=1,2,\""), plmn, GF("\",7"));
        // C. Importante: La búsqueda manual toma tiempo. Le damos hasta 30 segundos.
        modem.waitResponse(30000L, at_response); 
        local_logger("Registro manual: ", at_response);
    }
    // ==========================================================

    // Check network registration status and network signal status
    int16_t sq ;
    #ifdef DEBUG
        Serial.println("Wait for the modem to register with the network.");
    #endif
    RegStatus status = REG_NO_RESULT;
    k = 60;
    while ((status == REG_NO_RESULT || status == REG_SEARCHING || status == REG_UNREGISTERED) && (k > 0)) {
        if (phase_deadline_hit(phase_start, NET_PHASE_BUDGET_MS)) {
            local_logger("init_network deadline");
            return false;
        }
        k--;
        status = modem.getRegistrationStatus();
        switch (status) {
            case REG_UNREGISTERED:
                local_logger("REG_UNREGISTERED . . .");
                if (show_status) {
                    blink_led(0, 255, 0);
                } else {
                    delay(1000);
                }
                break;
            case REG_SEARCHING:
                #ifdef DEBUG
                    sq = modem.getSignalQuality();
                    local_logger("Signal Quality: ", sq);
                #endif
                if (show_status) {
                    blink_led(0, 255, 0);
                } else {
                    delay(1000);
                } 
                break;
            case REG_EMERGENCY_ONLY:
                local_logger("Attached for emergency bearer services only");
                if (show_status) {
                    blink_led(0, 255, 0);
                }                
                break;
            case REG_DENIED:
                local_logger("Network registration was rejected, please check if the APN is correct");
                return false;
            case REG_OK_HOME:
                local_logger("Online registration successful");                
                break;
            case REG_OK_ROAMING:
                local_logger("Network registration successful, currently in roaming mode");
                break;
            default:
                local_logger("Registration Status: ", status);
                if (show_status) {
                    blink_led(0, 255, 0);
                } else {
                    delay(1000);
                }
                break;
        }
    }

    if (status == REG_NO_RESULT || status == REG_SEARCHING || status == REG_UNREGISTERED) {
        local_logger("REGISTRATION FAILED!");
        if (show_status) {
            blink_led(255, 0, 0, 10000);
        }
        // If registration failed restart the modem and try again later
        modem.restart();
        return result;
    }

    local_logger("Registration Status: ", status);
    if (show_status) {
        blink_led(0, 255, 0);
    }                

    //For Roaming Enabling the network din't work. We use wait for network
    bool enable_network = false;
    String ipAddress = "";
    if (status != REG_OK_ROAMING && status != REG_EMERGENCY_ONLY) {
        k = 60;
        enable_network = modem.enableNetwork();
        while (!enable_network && k > 0) {
            if (phase_deadline_hit(phase_start, NET_PHASE_BUDGET_MS)) {
                local_logger("init_network deadline");
                return false;
            }
            enable_network = modem.enableNetwork();
            local_logger("Trying to enable network: ", k); 
            if (show_status) {
                blink_led(0, 255, 0);
            } 
            k--;
        }
        if (enable_network) {  
            local_logger("GET IP:");
            ipAddress = modem.getLocalIP();
            ipAddress.trim();
            #ifdef DEBUG
                if (ipAddress.isEmpty()) {
                    local_logger("IP EMPTY . . .");                    
                    delay(1000);
                }           
            #endif
        }
        if (show_status) {
            blink_led(0, 255, 0);
        } 
    } else {
        local_logger("TRY TO ENABLE NETWORK: ");
        k = 10;          
        while (ipAddress.isEmpty() && k > 0) {
            if (phase_deadline_hit(phase_start, NET_PHASE_BUDGET_MS)) {
                local_logger("init_network deadline");
                return false;
            }
            k--;
            #ifdef DEBUG
                Serial.println("");
                Serial.println("################################################################");
                Serial.print("TRY NUMBER: ");
                Serial.println(k);            
            #endif                        
            int p = 5;
            bool enable_network = modem.enableNetwork();
            while (!enable_network && p > 0) {
                if (phase_deadline_hit(phase_start, NET_PHASE_BUDGET_MS)) {
                    local_logger("init_network deadline");
                    return false;
                }
                p--;
                #ifdef DEBUG
                    Serial.println("");
                    Serial.println(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");                
                    Serial.println("");
                    Serial.print("Trying to enable network:");
                    Serial.println(p);
                    Serial.println("");
                #endif                
                enable_network = modem.enableNetwork();
                Serial.println("");
                delay(1000);                
            }
            if (enable_network) {  
                #ifdef DEBUG
                    Serial.println("");         
                    Serial.println("//////////////////////////////////////////////////////////////////");     
                    Serial.println("GET IP:");
                #endif
                ipAddress = modem.getLocalIP();
                ipAddress.trim();
                #ifdef DEBUG
                    if (ipAddress.isEmpty()) {
                        Serial.println("IP EMPTY . . .");                    
                        delay(1000);
                    }           
                #endif
            }
            if (show_status) {
                blink_led(0, 255, 0);
            }
        }    
    }

    result = !ipAddress.isEmpty();

    #ifdef DEBUG
        if (result) {
            modem.sendAT(GF("+CNUM"));
            if (modem.waitResponse(3000, at_response) != 1) {
                Serial.println("Read Phone Number Failed!");        
            }  else {
                Serial.print("RESPONSE CNUM: ");
                at_response.replace("AT+CNUM", "");  
                at_response.replace("+CNUM:", "");            
                at_response.replace(GSM_NL "OK" GSM_NL, "");
                at_response.replace(GSM_NL, "");
                at_response.trim();        
                Serial.println(at_response);        
            }
            Serial.print("Network IP:"); 
            Serial.println(ipAddress);    
            Serial.println("OPERATOR: ");
            Serial.print(modem.getOperator());
            delay(1000);
        } else {
            Serial.println("NETWORK ACTIVATION FAILED!!!");
            delay(5000);
        }
    #endif
    
    if (!result && show_status){
        blink_led(255, 0, 0, 10000);
    } 

    #ifdef DEBUG
        // MODO DEBUG: Forzamos la rotación de red en cada reinicio/ejecución
        current_network_index++;
        if (current_network_index > 3) {
            current_network_index = 0;
        }
        local_logger("DEBUG MODE: Forzando cambio manual de red a indice: ", current_network_index);        
    #endif


    return result;    
}

// Get Sensor Payload
// NUEVO: Se agregó el parámetro 'String value_types[]'
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

// Get Sensor Configuration from Server
bool get_sensor_configuration(bool show_status=false) {

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

// Clear Sensors Configuration from Preferences
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


// Prepare Async Sensors: Trigger and Wait for Ready
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
                local_logger("Fallo al leer estado del sensor:", namespace_id[i].c_str());
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
                        local_logger("Fallo al leer estado del sensor durante la espera:", namespace_id[i].c_str());
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
        local_logger("TIMEOUT CRÍTICO: El sensor no alcanzó el estado de Listo.");
        add_sensor_error("[ASYNC_TIMEOUT]");
        add_sensor_error("[ASYNC_LAST_STATUS]:[" + String(status) + "]");        
    }    
}

// Get Final Sensor Payload
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
        local_logger("ERROR CRITICO: Payload de sensores nulo. Se encola un registro de error.");
        add_sensor_error("[NULL_PAYLOAD_ERROR]");
        sensors_payload = "{\"err\":\"NULL_PAYLOAD\",\"n\":" + String((int)consecutive_null_errors) + "}";
    } else if (consecutive_null_errors > 0) {
        local_logger("Sensores recuperados. Limpiando contador de fallos RTC a 0.");
        consecutive_null_errors = 0;
        rtc_persist_commit();
    }

    doc["sp"] = serialized(sensors_payload);
    doc["gps_p"] = serialized(gps_payload);

    doc["fv"] = firmware_version;
    doc["uid"] = String(ESP.getEfuseMac());
    doc["dt"] = String(active_device_type);
    doc["net"] = global_apn;
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
    // ==========================================

    serializeJson(doc, final_payload);

    local_logger("FINAL PAYLOAD GENERATED: " + final_payload);

    return final_payload;
}

// Get GPS Payload
String get_gps_position(bool show_status=false) {

    String gps_payload = "{\"fix\":-1, \"lat\": -1, \"lon\": -1, \"spd\": -1, \"alt\": -1, \"vsat\": -1, \"usat\": -1, \"acc\": -1, \"yr\": 2000, \"mo\": 1, \"dy\": 1, \"hr\": 0, \"mi\": 0, \"sc\": 0, \"src\": \"Error\"}";

    float lat2      = -1;
    float lon2      = -1;
    float speed2    = 0;
    float alt2      = 0;
    int   vsat2     = 0;
    int   usat2     = 0;
    float accuracy2 = 0;
    int   year2     = 0;
    int   month2    = 0;
    int   day2      = 0;
    int   hour2     = 0;
    int   min2      = 0;
    int   sec2      = 0;
    uint8_t fixMode = 0;    

    
    local_logger("Enabling GPS/GNSS/GLONASS");
    bool got_gps_data = false;
    int i = 10;
    bool enabled_gps = false;
    while (!enabled_gps && (i > 0)) {
        enabled_gps = modem.enableGPS(MODEM_GPS_ENABLE_GPIO);
        if (!enabled_gps) {
            local_logger(".");
            i--;
            if (show_status) {
                blink_led(0,0,255, 300);
            } else {
                delay(1000);
            }            
        }
    }

    if (!enabled_gps) {
        local_logger("GPS Enabling Failed!");
    } else {
        local_logger("GPS Enabled");
        modem.setGPSBaud(115200);
        int attempts = 15;
        if (gps_fail_streak >= 3) attempts = 5;
        i = attempts;
        while (i > 0) {
            local_logger("Requesting current GPS/GNSS/GLONASS location");
            got_gps_data = modem.getGPS(&fixMode, &lat2, &lon2, &speed2, &alt2, &vsat2, &usat2, &accuracy2,
                            &year2, &month2, &day2, &hour2, &min2, &sec2);                
            if (got_gps_data) {
                gps_payload = "{";
                gps_payload.concat("\"lat\":"); 
                gps_payload += String(lat2, 5);
                gps_payload.concat(",");
                gps_payload.concat("\"lon\":");                 
                gps_payload += String(lon2, 5);
                gps_payload.concat(",");
                gps_payload.concat("\"src\":"); 
                gps_payload.concat("\"GPS\"");
                gps_payload.concat("}");                
                break;
            } else {
                local_logger("Couldn't get GPS/GNSS/GLONASS location, retrying in 1s.: ", i);
                i--;
                if (show_status) {
                    blink_led(0,0,255);
                } else {
                    delay(1000);
                }                
            }
        }            
    }    
    Serial.println("DISABLE GPS . . .");
    modem.disableGPS();
    if (gps_fix_ok(lat2, lon2)){
        gps_fail_streak = 0;
        //Si los datos están en los rangos normales para Costa Rica ya no los buscamos más.            
        #ifdef DEBUG
            local_logger("GET a Long and Lat. . .");
            Serial.println("LONG: ");
            Serial.println(lon2);
            Serial.println("LAT: ");
            Serial.println(lat2);
        #endif
        get_gps_data = false;
        local_lat2 = lat2;
        local_lon2 = lon2;
        if (show_status) {
            blink_led(0,255,0);
        }
    } else {
        //If we don't get GPS data in the correct range, we decide to use a fix value
        get_gps_data = false;
        if (!get_gps_data && gps_fix_ok(local_lat2, local_lon2)) {
            gps_payload = "{";
            gps_payload.concat("\"lat\":"); 
            gps_payload += String(local_lat2, 5);
            gps_payload.concat(",");
            gps_payload.concat("\"lon\":"); 
            gps_payload += String(local_lon2, 5);
            gps_payload.concat(",");
            gps_payload.concat("\"src\":"); 
            gps_payload.concat("\"Cache\"");
            gps_payload.concat("}");  
            local_logger("GPS Data from Cache!");
            local_logger(gps_payload);
            if (show_status) {
                blink_led(0,255,0);
            }
        }        
    }
    return gps_payload;
}

// Get GPS Payload with Cache
String get_gps_payload(bool show_status=false) {

    String gps_payload = "";
    #ifdef DEBUG
        Serial.println("Time Now: ");
        Serial.println(millis());
        Serial.println("Running Time Now: ");
        Serial.println(gps_running_time);
        Serial.println("CHECK_FIRMWARE_UPDATE: ");
        Serial.println(CHECK_FIRMWARE_UPDATE);
    #endif
    if (gps_running_time >= CHECK_GPS_UPDATE) {
        gps_running_time = 0;
        local_logger("Checking GPS Position");
        gps_payload = get_gps_position(show_status);
    } else if(!get_gps_data && gps_fix_ok(local_lat2, local_lon2)) {
        gps_payload = "{\"lat\":";
        gps_payload += String(local_lat2, 5);
        gps_payload.concat(", \"lon\":");
        gps_payload += String(local_lon2, 5);
        gps_payload.concat(", \"src\":");
        gps_payload.concat("\"Cache\"");
        gps_payload.concat("}");
        local_logger("GPS Data from Cache!");
        local_logger(gps_payload);
    } else {
        local_logger("Checking GPS Position");
        gps_payload = get_gps_position(show_status);
    }    
    return gps_payload;
}


/**
 * Try to send all queued data (RTC y SD)
 * processing by batch size MAX_BATCH_SIZE.
 */
bool send_all_queued_data() {
    if (!has_pending_data()) {
        local_logger("No hay datos pendientes por enviar.");
        return true; 
    }

    local_logger("Iniciando conexión de red para vaciar colas...");
    if (!init_mqtt_connection()) {
        local_logger("Fallo crítico: No se pudo conectar al Broker MQTT. Datos retenidos.");
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

        local_logger("Enviando lote paginado por MQTT...");
        if (publish_sensor_payload(batch_payload)) {
            // Éxito: Se eliminan definitivamente estos registros de la fuente (RTC o SD)
            commit_batch();
            batches_sent++; 
            local_logger("Lote enviado y confirmado con éxito.");
            
            // Un breve respiro para procesar respuestas del broker si es necesario
            comm.mqtt_handle(5000); 
        } else {
            // Fallo de red/comunicación durante el envío
            local_logger("Error al publicar el lote actual. Ejecutando Rollback...");
            rollback_batch(); // Si venía de RTC y la SD está lista, los "derrama" a la SD
            all_batches_ok = false;
            break; // Abortamos el bucle para no seguir perdiendo paquetes
        }
    }

    return all_batches_ok;
}

// The setup function controls the main program flow and runs once at startup and every wakeup
static const uint8_t CELL_RETRY_EVERY = 6;

bool gps_fix_ok(float lat, float lon) {
    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) return false;
    if (lat == 0.0f && lon == 0.0f) return false;
    if (lat == -1.0f && lon == -1.0f) return false;
    return true;
}

void use_cell_client() {
    comm.setClient(&cell_client);
    g_rtc.last_transport = 0;
    g_rtc.wakes_since_cell = 0;
    rtc_persist_commit();
}

#ifdef FEATURE_WIFI
bool use_wifi_client() {
    feed_watchdog();
    if (!wifi_client.network_connected() && !wifi_client.init_network()) return false;
    comm.setClient(&wifi_client);
    g_rtc.last_transport = 1;
    if (g_rtc.wakes_since_cell < 255) g_rtc.wakes_since_cell++;
    rtc_persist_commit();
    return true;
}
#endif

bool connect_uplink(bool show_status);

void apply_pack_power(const String& payload) {
    float volts = -1;
    const char* keys[] = {"\"pack_v\":", "\"voltage\":", "\"vbat\":", "\"V\":"};
    for (int i = 0; i < 4 && volts < 0; i++) {
        int at = payload.indexOf(keys[i]);
        if (at < 0) continue;
        volts = payload.substring(at + strlen(keys[i])).toFloat();
    }
    PowerInput in = {};
    in.pack_volts = volts;
    in.pack_valid = volts > 5.0f && volts < 20.0f;
    in.previous = (PowerMode)saved_power_mode;
    in.backend_sleep_sec = TIME_TO_SLEEP_SEC;
    in.backend_reading_count = READING_COUNT;
    PowerDecision decision = decide_power(in);
    active_power_mode = (uint8_t)decision.mode;
    active_sleep_sec = decision.eff_sleep_sec;
    announce_power_mode = decision.changed;
    saved_power_mode = decision.mode;
    last_pack_volts = in.pack_valid ? volts : -1;
    if (!in.pack_valid) {
        local_logger("Pack voltage missing or failed. Keeping the backend interval.");
        add_sensor_error("[PACK_V_MISSING]");
    }
}

bool connect_uplink(bool show_status) {
#ifdef FEATURE_WIFI
    bool prefer_wifi = (g_rtc.last_transport == 1 && g_rtc.wakes_since_cell < CELL_RETRY_EVERY);
    if (prefer_wifi && use_wifi_client()) return true;
#endif
    local_logger("Initializing Network Try 1 LTE");
    bool ok = init_network(MODEM_NETWORK_LTE, show_status);
    feed_watchdog();
    if (!ok) {
        local_logger("Initializing Network Try 2 LTE");
        ok = init_network(MODEM_NETWORK_AUTO, show_status);
        feed_watchdog();
        if (!ok) {
            local_logger("Initializing Network Try 1 GSM");
            ok = init_network(MODEM_NETWORK_GSM, show_status);
            feed_watchdog();
        }
    }
    if (ok) {
        use_cell_client();
        return true;
    }
#ifdef FEATURE_WIFI
    if (use_wifi_client()) return true;
#endif
    comm.setClient(&cell_client);
    return false;
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

void setup() {
    bool has_been_configured = true;
    init_watchdog();
    g_wake_start_ms = millis();
    rtc_persist_boot();
    ota_note_boot();
    comm.setClient(&cell_client);
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        load_device_params();
    }

    Serial.begin(115200);
    Serial.println(ESP.getEfuseMac());
    gpio_hold_dis((gpio_num_t)PIN_5V_EN);
    gpio_deep_sleep_hold_dis();
    pinMode(PIN_5V_EN, OUTPUT);
    digitalWrite(PIN_5V_EN, HIGH);
 
    if (rtc_reset_was_panic()) {
        local_logger("Panic or watchdog reset. Forcing OTA rescue.");
        init_modem();
        if (connect_uplink(false)) {
            check_for_firmware_update(true);
        }
        rtc_panic_flag = 0;
        rtc_persist_commit();
    }

    bool result = false;

    readingCnt++;

    if (first_run) {
        delay(30000);
        feed_watchdog();
    }    

    queue_manager_init();

    init_relay();
    feed_watchdog();

    if (first_run) {
        feed_watchdog();
        if (!modem_was_powered) init_modem();
        result = connect_uplink(first_run);
        
        if (result) {
            check_for_firmware_update(false);
#ifdef FEATURE_WIFI
            if (comm.isWifi()) reliable_time_sync_wifi();
            else reliable_time_sync(modem);
#else
            reliable_time_sync(modem);
#endif
            feed_watchdog();

            if (first_run) {
                bool fetched = get_sensor_configuration(first_run);
                feed_watchdog();
                has_been_configured = fetched || sensor_config_present();
            }
        }
    }

    // Automatic Reset Rain Sensor
    checkAndResetRainSensor();

    // Init RS485 Modbus
    init_peripherals();

    // Set panic flag in RTC to recover in case of WDT reset during the critical section of sensor reading and data processing, 
    //we will clear it at the end of the cycle if everything goes well
    rtc_panic_flag = 1;
    rtc_persist_commit();

    // Give sensor current from battery and wait a bit for stabilization before reading them 
    turn_sensors_on();
    start_time = millis();

    // Start Async Sensors and Wait for them to be ready if we have any configured, 
    //if not it will do nothing and we will read them immediately in the next step
    prepare_async_sensors();
    feed_watchdog();

    // Wait for sensors to stabilize
    wait_for_sensors();
    feed_watchdog();

    // FASE 2: Recolección inmediata de todos los sensores listos
    String final_sensor_payload = get_final_sensor_payload();
    feed_watchdog();
    apply_pack_power(final_sensor_payload);

    // Shutdown sensors
    turn_sensors_off();

    String gps_payload = "{\"src\":\"Skipped\"}";
    if (modem_was_powered) {
        gps_payload = get_gps_payload(first_run);
        feed_watchdog();
    } else if (gps_fix_ok(local_lat2, local_lon2)) {
        gps_payload = "{\"lat\":" + String(local_lat2, 5) + ",\"lon\":" + String(local_lon2, 5) + ",\"src\":\"Cache\"}";
    }

    bool allow_radio = active_power_mode != POWER_CRITICAL || announce_power_mode;
    if (radio_skip_remaining > 0 && !announce_power_mode && !first_run) {
        radio_skip_remaining--;
        allow_radio = false;
    }
    if (!result && allow_radio && readingCnt >= (int)READING_COUNT) {
        init_modem();
        result = connect_uplink(false);
        if (result) {
#ifdef FEATURE_WIFI
            if (comm.isWifi()) reliable_time_sync_wifi();
            else reliable_time_sync(modem);
#else
            reliable_time_sync(modem);
#endif
            check_for_firmware_update(false);
            gps_payload = get_gps_payload(false);
        }
    }

    // Do SD Card Diagnosis and Repair if needed
    if (!is_sdcard_ok) {
        local_logger("SD Card not OK! Attempting to diagnose and repair...");
        is_sdcard_ok = diagnose_and_repair_sd();
    }
    
    // Get Final Payload
    if (!has_been_configured) {
        JsonDocument config_error;
        config_error["err"] = "CONFIGURATION_ERROR";
        config_error["res"] = false;
        if (final_sensor_payload.length() > 0 && final_sensor_payload.charAt(0) == '{') {
            config_error["sensor_payload"] = serialized(final_sensor_payload);
        } else {
            config_error["sensor_payload"] = final_sensor_payload;
        }
        final_sensor_payload = "";
        serializeJson(config_error, final_sensor_payload);
    }
    String payload_data = get_final_payload(final_sensor_payload, gps_payload, result);     

    // Save always (RTC / Ring Buffer / SD Spillover)
    queue_payload(payload_data);
    local_logger("Lectura actual registrada en el gestor modular de colas.");

    // =======================================================
    // Transmission Phase: Only if we reached the READING_COUNT threshold, we will try to send all the accumulated data in one batch, 
    // if not we will wait for the next cycle to accumulate more data and save energy by not turning on the modem
    // =======================================================
    if (readingCnt >= READING_COUNT) {        
        local_logger("Se alcanzó el READING_COUNT. Procesando transmisión masiva...");
        
        // Try to send all queued data (RTC and SD) with a robust mechanism that handles partial failures and retries
        if (result) {
            result = send_all_queued_data();
        }

        if (result) {
            readingCnt = 0;
            consecutive_upload_failures = 0;            
            local_logger("¡Fase de transmisión completada con éxito!");
            ota_confirm_image();            
        } else {
            local_logger("Fallo en la transmisión. Los datos quedan seguros localmente.");
            feed_watchdog();

            // Increase the consecutive upload failures counter and check if we need to trigger a hardware reset to recover from potential network issues
            consecutive_upload_failures++;
            consecutive_network_changes++;
            if (consecutive_upload_failures >= 3) radio_skip_remaining = 3;
            if (consecutive_upload_failures >= MAX_CONSECUTIVE_FAILURES) {
                local_logger("Demasiados fallos de envio consecutivos. Forzando reinicio de hardware!");
                consecutive_upload_failures = 0;
                consecutive_network_changes = 0;
                perform_restart();
            }
        }        
    } else {
        // No data transmission this cycle, just logging the reading count. The message was saved in the queue manager for local storage.
        local_logger("Lectura acumulada. Ciclo intermedio de energía ultrabaja (", readingCnt, "/", READING_COUNT, ").");
        feed_watchdog();
    }

    // =======================================================
    // Reset panic flag and go to sleep
    // =======================================================
    rtc_panic_flag = 0;
    first_run = 0;
    rtc_persist_commit();
    maybe_upload_logs();
    update_running_time(); 
    go_sleep();
}

// The loop function is not used because we are using deep sleep
void loop() {

}