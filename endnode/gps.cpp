#include "firmware_context.h"
#include "logger.h"
#include "app_utils.h"
#include "network.h"
#include "sensors.h"
#include "gps_module.h"
#include "gps_policy.h"
#include "power_policy.h"
#include "queue_manager.h"
#include "time_manager.h"
#include "ota_guard.h"
#include "csv_parse.h"
#include "daily_log.h"
#include "variant.h"
#include <Update.h>
#include <esp_task_wdt.h>

String get_gps_position(bool show_status) {

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
    bool accepted = false;
    int i = 10;
    bool enabled_gps = false;
    if (awake_budget_expired()) {
        local_logger("Awake budget used. Skipping GPS.");
        return "{\"src\":\"Skipped\"}";
    }
    while (!enabled_gps && (i > 0)) {
        if (awake_budget_expired()) break;
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
        // gps_sample() passes first_boot as show_status. The cache and the
        // failure streak are the same live values decide_gps_sample() uses.
        const bool first_boot = show_status;
        const bool cache_ok = gps_fix_ok(local_lat2, local_lon2);
        const uint32_t budget_sec = gps_budget_sec(first_boot, cache_ok, gps_fail_streak);
        if (gps_fail_streak >= 3) {
            local_logger("GPS short start, budget", budget_sec, "s");
        } else if (first_boot || !cache_ok) {
            local_logger("GPS cold start, budget", budget_sec, "s");
        } else {
            local_logger("GPS warm start, budget", budget_sec, "s");
        }
        const unsigned long full_ms = (unsigned long)budget_sec * 1000UL;
        const unsigned long budget_ms = cap_awake_wait_ms((uint32_t)full_ms);
        if (budget_ms < full_ms) {
            local_logger("GPS budget capped by the awake limit", (uint32_t)(budget_ms / 1000UL), "s");
        }
        const unsigned long started = millis();
        while ((millis() - started) < budget_ms) {
            if (awake_budget_expired()) break;
            feed_watchdog();
            const unsigned long poll_at = millis();
            got_gps_data = modem.getGPS(&fixMode, &lat2, &lon2, &speed2, &alt2, &vsat2, &usat2, &accuracy2,
                            &year2, &month2, &day2, &hour2, &min2, &sec2);
            if (got_gps_data && (fixMode == 2 || fixMode == 3) && gps_fix_ok(lat2, lon2)) {
                accepted = true;
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
            }
            const unsigned long elapsed_ms = millis() - started;
            if (elapsed_ms >= budget_ms) break;
            unsigned long wait_ms = (unsigned long)GPS_POLL_MS;
            const unsigned long spent = millis() - poll_at;
            if (spent >= wait_ms) wait_ms = 0;
            else wait_ms = wait_ms - spent;
            const unsigned long remain = budget_ms - elapsed_ms;
            if (wait_ms > remain) wait_ms = remain;
            if (wait_ms > 0) delay(wait_ms);
        }
        const uint32_t elapsed_sec = (uint32_t)((millis() - started) / 1000UL);
        if (accepted) local_logger("GPS fix in", elapsed_sec, "s");
        else local_logger("GPS timeout after", elapsed_sec, "s");
    }
    Serial.println("DISABLE GPS . . .");
    modem.disableGPS();
    if (accepted){
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
        local_logger(gps_payload);
        if (show_status) {
            blink_led(0,255,0);
        }
    } else {
        if (awake_budget_expired()) {
            local_logger("Awake budget used. GPS stopped.");
        } else {
            if (enabled_gps && gps_fail_streak < 255) gps_fail_streak++;
            daily_log_note_gps_fail();
        }
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
String get_gps_payload(bool show_status) {

    String gps_payload = "";
    #ifdef DEBUG
        Serial.println("Time Now: ");
        Serial.println(millis());
        Serial.println("Running Time Now: ");
        Serial.println(gps_running_time);
        Serial.println("CHECK_FIRMWARE_UPDATE: ");
        Serial.println(CHECK_FIRMWARE_UPDATE);
    #endif
    // gps_sample passes first_boot as show_status. That flag still blinks the
    // LED, and on this path it also forces a GNSS read when the modem is on.
    GpsSampleInput sample = {};
    sample.first_boot = show_status;
    sample.modem_ready = true;
    sample.get_gps_data = get_gps_data;
    sample.cache_ok = gps_fix_ok(local_lat2, local_lon2);
    sample.update_due = gps_running_time >= CHECK_GPS_UPDATE;
    if (decide_gps_sample(sample) == GPS_CACHE) {
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
        if (sample.update_due) gps_running_time = 0;
        if (sample.first_boot) local_logger("First boot. Checking GPS position.");
        else local_logger("Checking GPS Position");
        gps_payload = get_gps_position(show_status);
    }
    return gps_payload;
}
bool gps_fix_ok(float lat, float lon) {
    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) return false;
    if (lat == 0.0f && lon == 0.0f) return false;
    if (lat == -1.0f && lon == -1.0f) return false;
    return true;
}

String gps_sample(bool modem_ready, bool first_boot) {
    if (modem_ready) {
        String gps_payload = get_gps_payload(first_boot);
        feed_watchdog();
        return gps_payload;
    }
    GpsSampleInput sample = {};
    sample.first_boot = first_boot;
    sample.modem_ready = false;
    sample.get_gps_data = get_gps_data;
    sample.cache_ok = gps_fix_ok(local_lat2, local_lon2);
    sample.update_due = gps_running_time >= CHECK_GPS_UPDATE;
    if (decide_gps_sample(sample) == GPS_CACHE) {
        return "{\"lat\":" + String(local_lat2, 5) + ",\"lon\":" + String(local_lon2, 5) + ",\"src\":\"Cache\"}";
    }
    return "{\"src\":\"Skipped\"}";
}
