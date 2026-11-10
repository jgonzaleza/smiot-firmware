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
#include "daily_log.h"
#include "wake_policy.h"
#include <ArduinoJson.h>
#include <esp_system.h>

static void sync_clock() {
#ifdef FEATURE_WIFI
    if (comm.isWifi()) reliable_time_sync_wifi();
    else reliable_time_sync(modem);
#else
    reliable_time_sync(modem);
#endif
}

static void recover_if_uplink_stale() {
    uint32_t now = (uint32_t)get_current_timestamp();
    if (!uplink_absent_for(now, last_uplink_unix, UPLINK_STALE_SEC)) return;
    local_logger("No uplink for 2 hours. Resetting RTC and restarting onto the first_run path.");
    spillover_rtc_to_sd();
    radio_skip_clear(&radio_skip_remaining);
    last_uplink_unix = now;
    persist_reset_defaults();
    esp_restart();
}

void boot_start(WakeState& wake) {
    wake.configured = true;
    wake.uplink = false;
    wake.radio_skipped = false;
    init_watchdog();
    g_wake_start_ms = millis();
    rtc_persist_boot();
    ota_note_boot();
    comm.setClient(&cell_client);
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        load_device_params();
    }
    Serial.begin(115200);
    daily_log_boot((int)esp_reset_reason());
    Serial.println(ESP.getEfuseMac());
    gpio_hold_dis((gpio_num_t)PIN_5V_EN);
    gpio_hold_dis((gpio_num_t)RS485_EN_PIN);
    gpio_hold_dis((gpio_num_t)RESET_RAIN_SENSOR_PIN);
    gpio_hold_dis((gpio_num_t)BOARD_SD_CS_PIN);
#ifdef MODEM_RESET_PIN
    gpio_hold_dis((gpio_num_t)MODEM_RESET_PIN);
#endif
    gpio_deep_sleep_hold_dis();
    pinMode(PIN_5V_EN, OUTPUT);
    digitalWrite(PIN_5V_EN, HIGH);
    recover_if_uplink_stale();

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
    bool want_radio = !wake.uplink && readingCnt >= (int)READING_COUNT;
    bool allow_radio = active_power_mode != POWER_CRITICAL || announce_power_mode;
    if (want_radio && allow_radio &&
        radio_skip_this_wake(&radio_skip_remaining, announce_power_mode, first_run != 0)) {
        allow_radio = false;
        daily_log_net_reason("skip");
    } else if (want_radio && !allow_radio) {
        daily_log_net_reason("critical");
    }
    bool attempted = false;
    if (want_radio && allow_radio) {
        attempted = true;
        init_modem();
        wake.uplink = connect_uplink(false);
        if (wake.uplink) {
            sync_clock();
            check_for_firmware_update(false);
            wake.gps = gps_sample(true, false);
        }
    }
    wake.radio_skipped = want_radio && !attempted;
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
        RadioSkipAccount radio;
        radio.skip_remaining = radio_skip_remaining;
        radio.consecutive_failures = consecutive_upload_failures;
        RadioAccountKind kind = account_radio_wake(&radio, sent, wake.radio_skipped, MAX_CONSECUTIVE_FAILURES);
        radio_skip_remaining = radio.skip_remaining;
        consecutive_upload_failures = radio.consecutive_failures;
        if (kind == RADIO_ACCOUNT_SUCCESS) {
            readingCnt = 0;
            note_uplink_success();
            uint32_t now = (uint32_t)get_current_timestamp();
            if (now != 0) last_uplink_unix = now;
            local_logger("Transmit phase completed.");
        } else if (kind == RADIO_ACCOUNT_SKIPPED) {
            local_logger("Radio skipped. This wake does not count as an upload failure.");
            feed_watchdog();
        } else {
            local_logger("Transmit failed. The readings stay stored locally.");
            feed_watchdog();
            note_uplink_failure();
            daily_log_note_net_fail();
        }
        ota_account_uplink(first_run != 0, kind == RADIO_ACCOUNT_SUCCESS);
        if (kind == RADIO_ACCOUNT_RESTART) {
            local_logger("Too many consecutive send failures. Restarting.");
            consecutive_network_changes = 0;
            perform_restart();
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
    write_daily_wake_log();
    maybe_upload_logs();
    update_running_time();
    go_sleep();
}
