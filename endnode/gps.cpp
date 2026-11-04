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
        modem.setGPSBaud(115200);
        int attempts = 15;
        if (gps_fail_streak >= 3) attempts = 5;
        i = attempts;
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
    if (gps_fix_ok(lat2, lon2)){
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
        if (show_status) {
            blink_led(0,255,0);
        }
    } else {
        daily_log_note_gps_fail();
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
