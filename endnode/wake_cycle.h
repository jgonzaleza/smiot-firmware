#pragma once
#include <Arduino.h>

struct WakeState {
    bool configured = true;
    bool uplink = false;
    String sensors;
    String gps;
};

void boot_start(WakeState& wake);
void network_provision(WakeState& wake);
bool network_transmit_if_needed(WakeState& wake);
void deliver_sample(WakeState& wake);
void sleep_now();
