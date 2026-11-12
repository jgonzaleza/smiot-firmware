#pragma once
#include <Arduino.h>
#include <TinyGsmClient.h>

void init_modem();
void power_off_modem_for_sleep();
bool init_network(NetworkMode mode_setup_value, bool show_status = false);
void use_cell_client();
#ifdef FEATURE_WIFI
bool use_wifi_client();
#endif
bool connect_uplink(bool show_status = false);
void note_uplink_success();
void note_uplink_failure();
