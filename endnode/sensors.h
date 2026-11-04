#pragma once
#include <Arduino.h>

void init_peripherals();
void wait_for_sensors();
void checkAndResetRainSensor();
String get_sensor_payload(String sensor_id, int baud_rate, int sensor_address, int start_holding_register, int registers_to_read, String value_names[], String value_types[], int value_names_size, bool has_types);
bool get_sensor_configuration(bool show_status = false);
bool sensor_config_present();
void clear_sensors_configuration();
void prepare_async_sensors();
String get_final_sensor_payload();
String sensors_sample(bool* configured);
// Writes the MEASURE_SCALES entry for this name. False when the backend sent none.
bool sensor_measure_scale_find(const char* value_name, float* out);
// Scale saved from the config document's MEASURE_SCALES. 1 when that name has none.
float sensor_measure_scale(const char* value_name);
