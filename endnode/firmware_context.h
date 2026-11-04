#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <HardwareSerial.h>
#include "tinygsm_yield.h"
#include <TinyGsmClient.h>
#include "CommunicationManager.h"
#include "CellularCommunicationClient.h"
#ifdef FEATURE_WIFI
#include "WifiCommunicationClient.h"
#endif
#include "rtc_persist.h"
#include "power_policy.h"
#include "sketch_config.h"
#include "logger.h"

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

#ifndef DEBUG
#define Serial if (false) Serial
#endif

extern TinyGsm modem;
extern CommunicationManager comm;
extern CellularCommunicationClient cell_client;
#ifdef FEATURE_WIFI
extern WifiCommunicationClient wifi_client;
#endif
extern Preferences preferences;
extern HardwareSerial Serial485;
extern bool modem_was_powered;
extern uint32_t active_sleep_sec;
extern uint8_t active_power_mode;
extern bool announce_power_mode;
extern int max_backlog_batches;
extern String configured_apn;
extern bool configured_apn_set;
extern uint8_t radio_skip_remaining;
extern uint8_t gps_fail_streak;
extern int saved_power_mode;
extern float last_pack_volts;
extern int last_rssi;
extern int active_device_type;
extern const int firmware_version;
extern char plmn_table[4][8];
extern long timezone_offset_sec;
extern String global_apn;
extern String sim_label;
extern String sim_iccid;
extern String registered_plmn;
extern bool sim_unknown_reported;
extern uint32_t net_sticky_since;
extern const char* broker;
extern const uint16_t broker_port;
extern const char* client_id_prefix;
extern const uint8_t client_index;
extern String publish_topic_prefix;
extern String subscribe_topic_prefix;
extern const char* CHECK_FIRMWARE_UPDATE_URL;
extern const char* CONFIGURATION_SERVER_URL;
extern const uint32_t OTA_TIMEOUT;
extern unsigned long CHECK_FIRMWARE_UPDATE;
extern unsigned long firmware_running_time;
extern uint32_t TIME_TO_SLEEP_SEC;
extern unsigned int TIME_TO_READ_SENSOR;
extern unsigned int READING_COUNT;
extern unsigned int TIME_BETWEEN_READINGS;
extern unsigned int READING_RATE;
extern bool has_async_sensors;
extern bool is_sdcard_ok;
extern char rtc_sd_error_log[256];
extern char rtc_sensor_error_log[256];
extern unsigned long start_time;
extern unsigned long g_wake_start_ms;
extern bool get_gps_data;
extern float local_lat2;
extern float local_lon2;
extern unsigned long gps_running_time;
extern unsigned long CHECK_GPS_UPDATE;
extern bool reset_rain_sensor;
extern const int namespaces_size;
extern String namespace_id[];

void load_device_params();
void check_for_firmware_update(bool force_check);
void perform_restart();
void update_running_time();
void go_sleep();
void maybe_upload_logs();
void apply_pack_power(const String& payload);
String get_final_payload(String sensors_payload, String gps_payload, bool result);
bool send_all_queued_data();
void turn_sensors_on();
void turn_sensors_off();
void init_relay();
