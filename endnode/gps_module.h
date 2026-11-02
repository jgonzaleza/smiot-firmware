#pragma once
#include <Arduino.h>

bool gps_fix_ok(float lat, float lon);
String get_gps_position(bool show_status = false);
String get_gps_payload(bool show_status = false);
String gps_sample(bool modem_ready, bool first_boot);
