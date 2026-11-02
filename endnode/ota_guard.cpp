#include "ota_guard.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

void ota_remember_slot() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    Preferences prefs;
    if (!prefs.begin("otarol", false) || !running || !running->label) return;
    prefs.putString("prev", running->label);
    prefs.putUChar("tries", 0);
    prefs.end();
}

void ota_note_boot() {
    Preferences prefs;
    if (!prefs.begin("otarol", false)) return;
    uint8_t tries = prefs.getUChar("tries", 0);
    String prev = prefs.getString("prev", "");
    if (tries >= 3 && prev.length() > 0) {
        const esp_partition_t* previous = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, prev.c_str());
        const esp_partition_t* running = esp_ota_get_running_partition();
        prefs.putUChar("tries", 0);
        prefs.end();
        if (previous && running && previous != running) {
            esp_ota_set_boot_partition(previous);
            ESP.restart();
        }
        return;
    }
    prefs.putUChar("tries", (uint8_t)(tries + 1));
    prefs.end();
}

void ota_confirm_image() {
    Preferences prefs;
    if (!prefs.begin("otarol", false)) return;
    prefs.putUChar("tries", 0);
    prefs.end();
}
