#include "wake_cycle.h"
#include "firmware_context.h"
#include "network.h"
#include "sensors.h"
#include "gps_module.h"
#include "logger.h"
#include "app_utils.h"
#include "time_manager.h"
#include "ota_guard.h"
#include "queue_manager.h"
#include <ArduinoJson.h>

static void sync_clock() {
#ifdef FEATURE_WIFI
    if (comm.isWifi()) reliable_time_sync_wifi();
    else reliable_time_sync(modem);
#else
    reliable_time_sync(modem);
#endif
}

void boot_start(WakeState& wake) {
    wake.configured = true;
    wake.uplink = false;
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

    readingCnt++;
    if (first_run) {
        delay(30000);
        feed_watchdog();
    }
    queue_manager_init();
    init_relay();
    feed_watchdog();
}

void network_provision(WakeState& wake) {
    if (!first_run) return;
    feed_watchdog();
    if (!modem_was_powered) init_modem();
    wake.uplink = connect_uplink(first_run);
    if (!wake.uplink) return;
    check_for_firmware_update(false);
    sync_clock();
    feed_watchdog();
    bool fetched = get_sensor_configuration(first_run);
    feed_watchdog();
    wake.configured = fetched || sensor_config_present();
}

bool network_transmit_if_needed(WakeState& wake) {
    bool allow_radio = active_power_mode != POWER_CRITICAL || announce_power_mode;
    if (radio_skip_remaining > 0 && !announce_power_mode && !first_run) {
        radio_skip_remaining--;
        allow_radio = false;
    }
    if (!wake.uplink && allow_radio && readingCnt >= (int)READING_COUNT) {
        init_modem();
        wake.uplink = connect_uplink(false);
        if (wake.uplink) {
            sync_clock();
            check_for_firmware_update(false);
            wake.gps = gps_sample(true, false);
        }
    }
    return wake.uplink;
}

void deliver_sample(WakeState& wake) {
    if (!is_sdcard_ok) {
        local_logger("SD card is not healthy. Trying a repair...");
        is_sdcard_ok = diagnose_and_repair_sd();
    }
    String sensors = wake.sensors;
    if (!wake.configured) {
        JsonDocument config_error;
        config_error["err"] = "CONFIGURATION_ERROR";
        config_error["res"] = false;
        if (sensors.length() > 0 && sensors.charAt(0) == '{') {
            config_error["sensor_payload"] = serialized(sensors);
        } else {
            config_error["sensor_payload"] = sensors;
        }
        sensors = "";
        serializeJson(config_error, sensors);
    }
    String payload_data = get_final_payload(sensors, wake.gps, wake.uplink);
    queue_payload(payload_data);
    local_logger("Current reading stored in the queue.");

    if (readingCnt >= READING_COUNT) {
        local_logger("Reading count reached. Starting the transmit phase...");
        bool sent = false;
        if (wake.uplink) sent = send_all_queued_data();
        if (sent) {
            readingCnt = 0;
            consecutive_upload_failures = 0;
            local_logger("Transmit phase completed.");
            ota_confirm_image();
        } else {
            local_logger("Transmit failed. The readings stay stored locally.");
            feed_watchdog();
            consecutive_upload_failures++;
            consecutive_network_changes++;
            if (consecutive_upload_failures >= 3) radio_skip_remaining = 3;
            if (consecutive_upload_failures >= MAX_CONSECUTIVE_FAILURES) {
                local_logger("Too many consecutive send failures. Restarting.");
                consecutive_upload_failures = 0;
                consecutive_network_changes = 0;
                perform_restart();
            }
        }
    } else {
        local_logger("Reading stored. Intermediate low-power cycle (", readingCnt, "/", READING_COUNT, ").");
        feed_watchdog();
    }
}

void sleep_now() {
    rtc_panic_flag = 0;
    first_run = 0;
    rtc_persist_commit();
    maybe_upload_logs();
    update_running_time();
    go_sleep();
}
