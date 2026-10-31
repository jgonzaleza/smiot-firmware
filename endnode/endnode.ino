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
 */

#define TINY_GSM_RX_BUFFER 1024 // Set RX buffer to 1Kb


// See all AT commands, if wanted
//#define DUMP_AT_COMMANDS
//#define DEBUG true

// Include Headers
#include "utilities.h"     // Board specific pin definitions
#include "error_codes.h"
#include "logger.h"
#include "app_utils.h"
#include "custom_pins.h"
#include "time_manager.h"
#include "queue_manager.h"

// External Libraries
#include <ModbusMaster.h>
#include <TinyGsmClient.h>
#include <HardwareSerial.h>
#include <esp_adc_cal.h>
#include <ArduinoJson.h>
#include <ctime> 
#include <Update.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <string.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <esp_task_wdt.h>
#include <vector>

// Security Certificates
#include "certs/AWSClientCertificate.h"
#include "certs/AWSClientPrivateKey.h"
#include "certs/AmazonRootCA.h"


// ================= CONSTANTS & CONFIG ================= //

// Network
#define DEFAULT_NETWORK_APN "hologram" // Default APN
const char *hologram_apn = "hologram"; 
const char *hologram_id = "89464";
const char *emnify_apn = "emnify";
const char *emnify_id = "89883";
String global_apn = DEFAULT_NETWORK_APN;
RTC_DATA_ATTR int current_network_index = 0; // Operator index(ICE, Liberty, Claro, Movistar, etc) used for the cellular connection, 
                                             // we will cycle through them in case of repeated upload failures to try to recover 
                                             // connectivity (Zombie Network Recovery)
const char* CR_NETWORKS[4] = {"AUTO", "71203", "71204", "71201"}; // We will use AUTO (which cycles through all available networks) 
                                                                  // as the default, but we can switch to specific ones if needed in 
                                                                  // case of connectivity issues to try to recover (Zombie Network Recovery) 
RTC_DATA_ATTR int consecutive_upload_failures = 0; // Counter for consecutive upload failures
RTC_DATA_ATTR int consecutive_network_changes = 0; // Counter for consecutive network changes
RTC_DATA_ATTR int consecutive_null_errors = 0; // Counter for consecutive null pointer errors during upload
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
const char *CHECK_FIRMWARE_KEY = "";
const char *BASE_FIRMWARE_UPDATE_URL = "https://alxv44fafe.execute-api.us-east-1.amazonaws.com/dev/";
const char *FIRMWARE_UPDATE_KEY = "";
const char *CONFIGURATION_SERVER_URL =  "https://irwouyd9p6.execute-api.us-east-1.amazonaws.com/pro/device_sensor_config"; // Configuration Server details
const char *CONFIGURATION_SERVER_KEY = "";

//Firmware Update Parameters
#define DEFAULT_CHECK_FIRMWARE_UPDATE 86400000L // Check every 24 hours
const uint32_t OTA_TIMEOUT = 30000L; // 30 seconds network timeout
RTC_DATA_ATTR unsigned long CHECK_FIRMWARE_UPDATE = DEFAULT_CHECK_FIRMWARE_UPDATE;
RTC_DATA_ATTR unsigned long firmware_running_time;


//Device Type ID
const int device_type = 2; //1: Compact Weather Station; 2: Compact Weather Station Configurable; 3: EndNode Configurable
//Firmware version defined in platformio.ini, should be updated with every firmware change to allow for proper OTA updates
const int firmware_version = FIRMWARE_VERSION; 

// Timing (Seconds)
#define uS_TO_S_FACTOR 1000000ULL
#define DEFAULT_SLEEP_SEC 510 // Default sleep time between measurements (5 mins)
RTC_DATA_ATTR uint32_t TIME_TO_SLEEP_SEC = DEFAULT_SLEEP_SEC; // Time to sleep between measurements
RTC_DATA_ATTR unsigned int readingCnt = 0; 

// Queue and Storage
#define maximumReadings 2000 // The maximum number of readings that can be stored in the available space

//Sensor Default Config
#define DEFAULT_BAUD_RATE 9600
#define DEFAULT_TIME_TO_SLEEP       540
#define DEFAULT_TIME_TO_READ_SENSOR     12000L
#define DEFAULT_READING_COUNT           1
#define DEFAULT_TIME_BETWEEN_READINGS   1 // Time between readings in seconds
#define DEFAULT_READING_RATE            1 // How many readings
#define DEFAULT_CHECK_FIRMWARE_UPDATE 86400000L
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

// Init parameters
RTC_DATA_ATTR bool first_run = true;
unsigned long start_time = 0;
RTC_DATA_ATTR long lastResetDayNumber = -1; // Used for automatic rain sensor reset logic, initialized to -1 to ensure it runs on the first day
RTC_DATA_ATTR int rtc_panic_flag = 0; // Flag to indicate if the last reset was due to a panic (watchdog timeout)

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

// Global Flags
bool reset_rain_sensor = false; //Flag to reset rain sensor

//We only support 8 configuration for sensors
const int namespaces_size = 8;
String namespace_id[namespaces_size] = {"SENSOR_1", "SENSOR_2", "SENSOR_3", "SENSOR_4", "SENSOR_5", "SENSOR_6", "SENSOR_7", "SENSOR_8"};

// ================= FUNCTION DEFINITIONS ================= //


// Send Sensor Payload via MQTT
bool publish_sensor_payload(String payload) {
    
    bool result = false;
    if (modem.mqtt_connected()) {
        // If connected, send information          
        local_logger("Publish Current Data");
        String publish_topic = publish_topic_prefix + String(ESP.getEfuseMac());
        result = modem.mqtt_publish(client_index, publish_topic.c_str(), payload.c_str());
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
    local_logger("MQTT Begin . . .");
    // Initialize MQTT, use SSL
    modem.mqtt_begin(true);

    local_logger("MQTT Set Certificates . . .");
    // Set Amazon Certificate
    modem.mqtt_set_certificate(AmazonRootCA, AWSClientCertificate, AWSClientPrivateKey);
    
    local_logger("MQTT Connecting . . .");
    // Connecting to AWS IOT Core
    mqtt_connect_result = mqtt_connect();

    int k = 10;
    while (!mqtt_connect_result && (k > 0)) {
        k--;
        // Check the connection every ten seconds
        #ifdef DEBUG
            Serial.println("Retrying MQTT Connect . . .");
        #endif        
        mqtt_connect_result = mqtt_connect();
        if (!mqtt_connect_result) {            
            if (k == 5){
                //Try to reset the connection
                modem.mqtt_disconnect(client_index);
                delay(1000);
                modem.mqtt_end();
                delay(1000);
                modem.mqtt_begin(true);
                modem.mqtt_set_certificate(AmazonRootCA, AWSClientCertificate, AWSClientPrivateKey);
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
    modem.mqtt_set_callback(get_mqtt_parameters);
    // Subscribe to topic
    String final_subscribe_topic = subscribe_topic_prefix + ESP.getEfuseMac();
    if (modem.mqtt_subscribe(client_index, final_subscribe_topic.c_str())) {
        local_logger("Subscribed to topic: ", final_subscribe_topic);
    } else {
        local_logger("MQTT Subscribe Failed!");
    }
}

// Wait for sensors to stabilize
void wait_for_sensors(){
    //IF SIM initialization and GPS lasted less than 15 sec, we want to give sensors at least 15 sec to stabilize
    // and get data from them.

    unsigned long end_time = millis();
    unsigned long elapsed_time = end_time - start_time;
    while (elapsed_time < TIME_TO_READ_SENSOR) {
        local_logger("Waiting for minimal sensor time . . .");
        delay(1000);
        end_time = millis();
        elapsed_time = end_time - start_time;
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
    long currentTimestampUtc = get_current_timestamp();

    // Fallback de seguridad por si falla la red/NTP en este ciclo.
    if (currentTimestampUtc <= 0) {
        return; 
    }

    // Calculamos el tiempo local (UTC-6)
    const long timezoneOffset = -21600; 
    long localTimestamp = currentTimestampUtc + timezoneOffset;

    // Calculamos el número de día actual (días transcurridos desde el epoch)
    long currentDayNumber = localTimestamp / 86400;

    // Caso 1: Arranque en frío (Cold Boot) o primera vez que se ejecuta.
    // El dispositivo se acaba de encender (se le puso la batería o hubo un apagón).
    // Inicializamos el día actual, pero NO reiniciamos el sensor para no borrar 
    // datos previos a menos que estemos seguros del cambio de día.
    if (lastResetDayNumber == -1) {
        lastResetDayNumber = currentDayNumber;
        local_logger("Cold boot: Initialized lastResetDayNumber to ", currentDayNumber);
        return;
    }

    // Caso 2: Despertar de Deep Sleep (Warm Boot).
    // Comparamos el día actual con el día guardado en la memoria RTC.
    if (currentDayNumber != lastResetDayNumber) {
        // Ha ocurrido un cambio de día, es hora de reiniciar el sensor de lluvia.

        // Actualizamos el registro en la memoria RTC para mañana
        lastResetDayNumber = currentDayNumber;
        reset_sensor();
        local_logger("Rain sensor reset for new day. Old Day: ", lastResetDayNumber, " New Day: ", currentDayNumber);
    }
}

// MQTT Connect Function
bool mqtt_connect() {
    local_logger("Connecting to ", broker);

    String dynamic_client_id = String(client_id_prefix) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    bool ret = modem.mqtt_connect(client_index, broker, broker_port, dynamic_client_id.c_str());
    if (!ret) {
        local_logger("Failed!"); return false;
    }

    if (modem.mqtt_connected()) {
        local_logger("MQTT has connected!");
    } else {
        return false;
    }
    return true;
}

// MQTT Disconnect Function
void mqtt_disconnect() {
    local_logger("MQTT Disconnecting . . .");
    modem.mqtt_disconnect(client_index);
    delay(500);
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
    char* payload_str = (char*)payload;
    JsonDocument parameters_payload;

    #ifdef DEBUG
        Serial.println("Payload: ");
        Serial.println(payload_str);
    #endif

    //Conver the String to JSON Document
    deserializeJson(parameters_payload, payload_str);

    //check if the payload contains the reset rain sensor command
    reset_rain_sensor = parameters_payload["RR_SENSOR"].as<bool>();
    if (reset_rain_sensor) {
        local_logger("⚠️ Reset rain sensor command received!");
        reset_rain_sensor = false;
        reset_sensor();
    }

    //Check remote restart command
    if (parameters_payload["RESTART"].as<bool>() == true) {
        local_logger("⚠️ Remote restart command received!");
        // Give the serial port time to finish printing the log
        delay(500);         
        // Hard Restart the ESP32
        perform_restart();
    }


    TIME_TO_SLEEP_SEC = parameters_payload["TTS_SEC"].as<int>();
    TIME_TO_READ_SENSOR = parameters_payload["TTR_SENSOR"].as<int>();
    READING_COUNT = parameters_payload["R_COUNT"].as<int>();
    TIME_BETWEEN_READINGS = parameters_payload["TB_READINGS"].as<int>();
    READING_RATE = parameters_payload["R_RATE"].as<int>();
    get_gps_data = parameters_payload["G_GPS_DATA"].as<bool>();
    CHECK_FIRMWARE_UPDATE = parameters_payload["C_F_UPDATE"].as<long>();
    local_lat2 = parameters_payload["L_LAT"].as<double>();
    local_lon2 = parameters_payload["L_LONG"].as<double>();            


    #ifdef DEBUG
        Serial.println("NEW TIME_TO_SLEEP_SEC: ");
        Serial.println(TIME_TO_SLEEP_SEC);
        Serial.println("NEW TIME_TO_READ_SENSOR: ");
        Serial.println(TIME_TO_READ_SENSOR);
        Serial.println("NEW READING_COUNT: ");
        Serial.println(READING_COUNT);
        Serial.println("NEW TIME_BETWEEN_READINGS: ");
        Serial.println(TIME_BETWEEN_READINGS);
        Serial.println("NEW READING_RATE: ");
        Serial.println(READING_RATE);
        Serial.println("NEW get_gps_data: ");
        Serial.println(get_gps_data);
        Serial.println("NEW CHECK_FIRMWARE_UPDATE: ");
        Serial.println(CHECK_FIRMWARE_UPDATE);
    #endif

    parameters_payload.clear();           
}

// Check Firmware Update Function
JsonDocument check_firmware_update(){

    JsonDocument parameters_payload;

    // Initialize HTTPS
    modem.https_begin();

    // Set GET URT
    if (!modem.https_set_url(CHECK_FIRMWARE_UPDATE_URL)) {
        Serial.println("Failed to set the URL. Please check the validity of the URL!");
        modem.https_end();
        return parameters_payload;
    } 
    // Send GET request
    modem.https_add_header("X-API-Key", CHECK_FIRMWARE_KEY);
    int httpCode = 0;
    String post_payload = "{\"client_id\": \"";
    post_payload.concat(client_id_prefix);
    post_payload.concat("\", \"device_type\":");
    post_payload.concat(device_type);
    post_payload.concat(", \"firmware_version\":");
    post_payload.concat(firmware_version);
    post_payload.concat(", \"device_id\": \"");
    post_payload.concat(ESP.getEfuseMac());
    post_payload.concat("\"}");
    httpCode = modem.https_post(post_payload);
    if (httpCode != 200) {
        Serial.print("HTTP POST failed ! error code = "); 
        Serial.println(httpCode); 
        modem.https_end();
        local_logger("HTTP get failed CHECK FIRMWARE UPDATE");
        return parameters_payload;
    }
    String body = modem.https_body();
    #ifdef DEBUG
        Serial.print("HTTP body : ");
        Serial.println(body);
    #endif

    //Conver the String to JSON Document
    deserializeJson(parameters_payload, body);

    modem.https_end();

    return parameters_payload;
}


// Firmware Update Function
void firmware_update(const char *bucket_name, const char *file_name){

    // Initialize HTTPS
    modem.https_begin();
    local_logger("Getting firmware file. . .");

    String final_url = BASE_FIRMWARE_UPDATE_URL;
    final_url.concat(bucket_name);
    final_url.concat("/");
    final_url.concat(file_name);
    local_logger("File Update URL: ");
    local_logger(final_url);
    if (!modem.https_set_url(final_url)) {
        Serial.println("Failed to set the URL. Please check the validity of the URL!");
        modem.https_end();
        local_logger("Failed to set the URL. Please check the validity of the URL!");
        return;
    } 

    //Set API Key
    modem.https_add_header("X-API-Key", FIRMWARE_UPDATE_KEY);
    int httpCode = 0;
    httpCode = modem.https_get();
    if (httpCode != 200) {
        Serial.print("HTTP GET failed ! error code = "); 
        Serial.println(httpCode); 
        local_logger("HTTP GET failed ! FIRMWARE UPDATE");
        modem.https_end();
        return;
    }
    local_logger("Reading firmware file. . .");
    size_t firmware_size = modem.https_get_size();
    
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
        int len = modem.https_body(buffer, 1024);
        
        if (len > 0) {
            written = Update.write(buffer, len);
            if (written != len) {
                Serial.println("Written only : " + String(written) + "/" + String(len));
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

    modem.https_end();

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

    #ifdef DEBUG
        Serial.println("Enter modem power off!");
    #endif
    int i = 60;
    while (!modem.poweroff() && i > 0) {
        #ifdef DEBUG
            Serial.print("Modem entering power off: ");
            Serial.println(i);
            Serial.println();
        #endif
        i--;
        delay(500);
    } 

    #ifdef DEBUG
        Serial.println("Waiting for modem power off");
    #endif
    delay(5000);
    int k = 50;
    while (modem.testAT() && (k > 0)) {
        delay(1000);
        k--;
    }
    #ifdef DEBUG
        Serial.println("Modem is not responding ,modem has power off !");        
    #endif

    //Try to low the current when deep sleep
    pinMode(PIN_5V_EN, OUTPUT);
    digitalWrite(PIN_5V_EN, LOW);
    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, LOW);
    
    if (rtc_gpio_isolate((gpio_num_t)PIN_5V_EN) == ESP_OK) {
        local_logger("PIN_5V_EN INSOLATED");
    }
    
 
    // Pull up DTR to put the modem into sleep
    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);

    #ifdef BOARD_POWERON_PIN
        // Turn off DC boost to power off the modem
        digitalWrite(BOARD_POWERON_PIN, LOW);
        //rtc_gpio_isolate((gpio_num_t)BOARD_POWERON_PIN);
    #endif

    #ifdef MODEM_RESET_PIN
        // Keep it low during the sleep period. If the module uses GPIO5 as reset, 
        // there will be a pulse when waking up from sleep that will cause the module to start directly.
        digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
        gpio_hold_en((gpio_num_t)MODEM_RESET_PIN);
        gpio_deep_sleep_hold_en();
    #endif

    #ifdef DEBUG
        Serial.println("Weather Station is going to  deep sleep!");
    #endif

    
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_SEC * uS_TO_S_FACTOR);
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

    // Turn on DC boost to power on the modem
    #ifdef BOARD_POWERON_PIN
        pinMode(BOARD_POWERON_PIN, OUTPUT);
        digitalWrite(BOARD_POWERON_PIN, HIGH);
    #endif

    gpio_hold_dis((gpio_num_t)PIN_5V_EN);

    #ifdef MODEM_RESET_PIN
        // Release reset GPIO hold
        gpio_hold_dis((gpio_num_t)MODEM_RESET_PIN);

        // Set modem reset pin ,reset modem
        // The module will also be started during reset.
        #ifdef DEBUG
            Serial.println("Set Reset Pin.");
        #endif
        pinMode(MODEM_RESET_PIN, OUTPUT);
        digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL); delay(100);
        digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL); delay(2600);
        digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
    #endif

    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, HIGH);
    #ifdef DEBUG
        Serial.println("Power on the modem PWRKEY.");
    #endif
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    //Ton >= 100 <= 500
    delay(100);
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

    //Disable WiFi
    WiFi.mode(WIFI_OFF);

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
    // Check if SIM card is online
    SimStatus sim = SIM_ERROR;
    bool result = false;
    int k = 30;
    while ((sim != SIM_READY) && (k > 0)) {
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
        String plmn = CR_NETWORKS[current_network_index];
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
            case REG_EMERGENCY:
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
    if (status != REG_OK_ROAMING && status != REG_EMERGENCY) {
        k = 60;
        enable_network = modem.enableNetwork();
        while (!enable_network && k > 0) {
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
        modem.https_begin();
        blink_led(255,255,255, 500);
    } else {
        modem.https_begin();
    }


    // Set GET URT
    if (!modem.https_set_url(CONFIGURATION_SERVER_URL)) {
        Serial.println("Failed to set the URL. Please check the validity of the URL!");
        modem.https_end();
        if (show_status) {
            blink_led(255,0,0,10000);
        }        
        return false;
    }
    if (show_status) {
        blink_led(255,255,255,500);
    }

    // Send POST request
    modem.https_add_header("X-API-Key", CONFIGURATION_SERVER_KEY);
    int httpCode = 0;
    String post_payload = "{\"device_type\": ";
    post_payload.concat(device_type);
    post_payload.concat(", \"device_id\": \"");
    post_payload.concat(device_id);
    post_payload.concat("\"}");
    httpCode = modem.https_post(post_payload);
    if (show_status) {
        blink_led(255,255,255,500);
    }
    if (httpCode != 200) {
        Serial.print("HTTP get failed ! error code = "); 
        Serial.println(httpCode);         
        modem.https_end();
        local_logger("HTTP get failed GET UPDATE PARAMETERS");
        if (show_status) {
            blink_led(255,0,0,10000);
        }
        return false;
    }

    if (show_status) {
        blink_led(255,255,255,500);
    }
    String body = modem.https_body();
    if (show_status) {
        blink_led(255,255,255,500);
    }
    #ifdef DEBUG
        Serial.print("HTTP body : ");
        Serial.println(body);
        delay(5000);
    #endif

    //Conver the String to JSON Document
    deserializeJson(parameters_payload, body);
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
    if (sensors.isNull() || sensors.size() <= 0) {
        if (show_status) {
            blink_led(255,0,0,10000);
        }
        return false;
    }

    //Preferences Keys max lenght is 15 caracters
    has_async_sensors = false;
    int i = 0;
    for (JsonVariant sensor_config: sensors) {
        #ifdef DEBUG
            Serial.println("NAMESPACE: ");        
            Serial.print(namespace_id[i].c_str());
            Serial.println(i);
        #endif
        preferences.begin(namespace_id[i].c_str());
        preferences.clear();
        String sensor_id = sensor_config["SENSOR_ID"].as<String>();
        preferences.putString("SENSOR_ID", sensor_id);

        // Check if configuration contains "IS_ASYNC"
        int is_async = sensor_config["IS_ASYNC"] | 0;
        preferences.putInt("IS_ASYNC", is_async);

        if (is_async == 1) {
            has_async_sensors = true;

            int trig_reg = sensor_config["TRIG_REG"] | 0;
            int trig_val = sensor_config["TRIG_VAL"] | 1;
            int stat_reg = sensor_config["STAT_REG"] | 1;
            int ready_val = sensor_config["READY_VAL"] | 3;
            int warmup_val = sensor_config["WARMUP_VAL"] | 1;
            uint32_t max_wait_ms = sensor_config["MAX_WAIT_MS"] | 60000;            
            
            // Save async parameters in preferences
            preferences.putInt("TRIG_REG", trig_reg);
            preferences.putInt("TRIG_VAL", trig_val);
            preferences.putInt("STAT_REG", stat_reg);
            preferences.putInt("READY_VAL", ready_val);
            preferences.putInt("WARMUP_VAL", warmup_val);
            preferences.putUInt("MAX_WAIT_MS", max_wait_ms);            
            local_logger("Sensor", sensor_id, "will be treated as ASYNC. Trigger Register:", trig_reg, "Trigger Value:", trig_val, "Status Register:", stat_reg, "Ready Value:", ready_val, "Warmup Value:", warmup_val, "Max Wait (ms):", max_wait_ms);
        } else {
            local_logger("Sensor", sensor_id, "will be treated as SYNC. No async parameters will be stored.");
        }

        // Store async configuration in preferences        
        preferences.putInt("SENSOR_ADDRESS", sensor_config["SENSOR_ADDRESS"].as<int>());
        preferences.putInt("START_HOLD_REG", sensor_config["START_HOLDING_REGISTER"].as<int>());
        preferences.putInt("REGS_TO_READ", sensor_config["REGISTERS_TO_READ"].as<int>());
        preferences.putInt("BAUD_RATE", sensor_config["BAUD_RATE"] | 9600); // 9600 default baud rate if not specified
        String value_names = sensor_config["VALUE_NAMES"].as<String>();
        preferences.putString("VALUE_NAMES", value_names);
        
        if (sensor_config["VALUE_TYPES"].is<String>()) {
            String value_types = sensor_config["VALUE_TYPES"].as<String>();
            preferences.putString("VALUE_TYPES", value_types);
            #ifdef DEBUG
                Serial.print("Tipos de variables guardados (VALUE_TYPES): ");
                Serial.println(value_types);
            #endif
        }

        preferences.end();
        i++;
        if (show_status) {
            blink_led(255,255,255,500);
        }

        #ifdef DEBUG
            Serial.println("sensor_id: ");
            Serial.println(sensor_id);
            delay(1000);
        #endif
    }

    //Release the Json Document
    parameters_payload.clear();
    modem.https_end();
    if (show_status) {
        blink_led(255,255,255,500);
    }
    return true;
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
    
    for (int k=0;k<READING_RATE;k++) {
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
                
                String value_names[10];
                int r=0,t=0;        
                for(int j=0;j<str_value_names.length();j++) {
                    if(str_value_names[j] == ',') {
                        if (j-r > 1) {
                            value_names[t] = str_value_names.substring(r,j);
                            t++;
                            r = j+1;
                        }
                    }
                }
                int value_names_size = t;

                // Separación simple de tipos
                String value_types[10];
                bool has_types = false;
                
                if (str_value_types.length() > 0) {
                    has_types = true;
                    int rt=0, tt=0;
                    for(int j=0; j<str_value_types.length(); j++) {
                        if(str_value_types[j] == ',') {
                            if (j-rt > 1) { // Prevención estándar de comas vacías
                                value_types[tt] = str_value_types.substring(rt,j);
                                tt++;
                                rt = j+1;
                            }
                        }
                    }
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
        delay(TIME_BETWEEN_READINGS);
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
        local_logger("ERROR CRÍTICO: Payload de sensores nulo detectado.");
        // Guardar el evento en la bitácora local (SD card) si está disponible
        if (consecutive_null_errors == 1 || consecutive_null_errors % 24 == 0) {
            local_logger("Payload nulo. Reiniciando el ESP32 (Fallo #" + String(consecutive_null_errors) + ").");
            delay(500); 
            perform_restart();       
        } else {
            local_logger("Payload nulo persistente. Omitiendo reinicio y red para ahorrar batería.");
            add_sensor_error("[NULL_PAYLOAD_ERROR]:[" + String(consecutive_null_errors) + "]");            
        } 
    } else {
        if (consecutive_null_errors > 0) {
            local_logger("Sensores recuperados. Limpiando contador de fallos RTC a 0.");
            consecutive_null_errors = 0;
        }
    }

    doc["sp"] = serialized(sensors_payload);
    doc["gps_p"] = serialized(gps_payload);

    doc["fv"] = firmware_version;
    doc["uid"] = String(ESP.getEfuseMac());
    doc["dt"] = String(device_type);
    doc["net"] = global_apn;
    doc["ts"] = (int)current_time_source;
    doc["tm"] = final_timestamp;

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
        // Set GPS Baud to 115200
        modem.setGPSBaud(115200);        
        //The first time we need half minute at least to get a position.
        for (int j=0;j<30;j++) {
            if (show_status) {
                blink_led(0,0,255);
            } else {
                delay(1000);
            }            
        }
        i = 60;
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
    if (lat2 >= 8 && lat2 <= 11.5 && lon2 >= 82 && lon2 <= 86){
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
        if (!get_gps_data && local_lat2 >= 8 && local_lat2 <= 11.5 && local_lon2 >= 82 && local_lon2 <= 86) {
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
    } else if(!get_gps_data && local_lat2 >= 8 && local_lat2 <= 11.5 && local_lon2 >= 82 && local_lon2 <= 86) {
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

    // Bucle de paginación: mientras existan registros en RTC o SD...
    while (has_pending_data()) {
        feed_watchdog(); // Alimentamos el WDT en cada lote por seguridad

        // Extrae un arreglo JSON de hasta MAX_BATCH_SIZE lecturas: "[{...},{...}]"
        String batch_payload = get_batch_payload(); 
        if (batch_payload == "") break;

        local_logger("Enviando lote paginado por MQTT...");
        if (publish_sensor_payload(batch_payload)) {
            // Éxito: Se eliminan definitivamente estos registros de la fuente (RTC o SD)
            commit_batch(); 
            local_logger("Lote enviado y confirmado con éxito.");
            
            // Un breve respiro para procesar respuestas del broker si es necesario
            modem.mqtt_handle(5000); 
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
void setup() {
    bool has_been_configured = true;
    // Inicializar Watchdog Timer
    init_watchdog();

    init_modem();

    Serial.println(ESP.getEfuseMac());
 
    // =======================================================
    // 1. PANIC RESTART (WDT Reset)
    // =======================================================
    if (rtc_panic_flag == 1) {
        local_logger("¡ALERTA! Reinicio por Pánico detectado. Forzando modo de Salvataje OTA.");        
        init_network(MODEM_NETWORK_LTE, false); // Forzamos conexión a red
        check_for_firmware_update(true);           // Intentamos bajar un parche
        rtc_panic_flag = 0;                    // Limpiamos la bandera
    }

    bool result = false;

    // Incremento exacto del ciclo actual (Corregido el doble incremento)
    readingCnt++;

    if (first_run) {
        // Delay to install the station.
        delay(30000);
        feed_watchdog();
    }    

    // Initialize the queue manager for the local storage
    queue_manager_init();

    // Initialize Relay
    init_relay();
    feed_watchdog();

    if (first_run) {
        // Reset rain sensor
        reset_sensor();
        feed_watchdog();
    }

    // Turn on the controller and try to connect to the network only if it's the first run or we reached the READING_COUNT threshold
    if (first_run || readingCnt >= READING_COUNT) {
        // Turn on the controller        
        feed_watchdog();

        // We try two times with LTE and once with GSM
        local_logger("Initializing Network Try 1 LTE");
        result = init_network(MODEM_NETWORK_LTE, first_run);
        feed_watchdog();
        
        if (!result) {            
            local_logger("Initializing Network Try 2 LTE");
            result = init_network(MODEM_NETWORK_AUTO, first_run);
            feed_watchdog();
            if (!result) {
                local_logger("Initializing Network Try 1 GSM");
                result = init_network(MODEM_NETWORK_GSM, first_run);
                feed_watchdog();
            }
        }
        
        if (result) {
            // Check for firmware update
            check_for_firmware_update(false);

            reliable_time_sync(modem);
            feed_watchdog();

            if (first_run) {
                // Clear any previous configuration
                clear_sensors_configuration();
                // Get sensor configuration                
                result = get_sensor_configuration(first_run);
                feed_watchdog();
                // Sensor configuration result is stored in the has_been_configured variable to be used later in the payload generation
                has_been_configured = result;
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

    // Give sensor current from battery and wait a bit for stabilization before reading them 
    turn_sensors_on();
    start_time = millis();

    // Get GPS Payload (Utilizará el caché RTC automáticamente si el módem está apagado)
    String gps_payload = get_gps_payload(first_run);
    feed_watchdog();

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

    // Shutdown sensors
    turn_sensors_off();

    // Do SD Card Diagnosis and Repair if needed
    if (!is_sdcard_ok) {
        local_logger("SD Card not OK! Attempting to diagnose and repair...");
        is_sdcard_ok = diagnose_and_repair_sd();
    }
    
    // Get Final Payload
    if (!has_been_configured) {
        final_sensor_payload = "{\"err\":\"CONFIGURATION_ERROR\",\"res\":false, \"sensor_payload\":\"" + final_sensor_payload + "\"}";
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
        } else {
            local_logger("Fallo en la transmisión. Los datos quedan seguros localmente.");
            feed_watchdog();

            // Increase the consecutive upload failures counter and check if we need to trigger a hardware reset to recover from potential network issues
            consecutive_upload_failures++;
            consecutive_network_changes++;
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

    first_run = false;
    update_running_time(); 
    go_sleep();
}

// The loop function is not used because we are using deep sleep
void loop() {

}