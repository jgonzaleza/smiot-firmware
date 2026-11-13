#ifndef APP_UTILS_H
#define APP_UTILS_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "tinygsm_yield.h"
#include <TinyGSM.h>
#include <esp_task_wdt.h>
#include "custom_pins.h"
#include "logger.h"
#include "utilities.h"
#include "rtc_persist.h"
#include "param_limits.h"
#include "daily_log.h"
#include "wake_policy.h"


// 5 Minutes. If the board gets stuck for 5 mins without "feeding", it restarts.
#define WDT_TIMEOUT_SECONDS 300 

// Define the file path constant if not already defined
// Usamos #pragma once o guardas de inclusión al inicio (ya lo tienes como #ifndef APP_UTILS_H)

// Las constantes de texto se manejan con 'extern' para evitar redefiniciones
static const char* QUEUE_FILE = "/queue.txt";
static const char* STAGING_FILE = "/staging.txt";
static const char* LEFTOVERS_FILE = "/leftovers.txt";

// SD Card ready flag debe ser externa para que el queue_manager la pueda leer
extern bool is_sdcard_ok;

// ==============================================================================
// DEBUG SD CARD LOGIC
// ==============================================================================
extern char rtc_sd_error_log[256];

// Add an error message to the SD card diagnostic log (stored in RTC)
inline void add_sd_error(String msg) {
    if (strlen(rtc_sd_error_log) + msg.length() < 255) {
        strcat(rtc_sd_error_log, msg.c_str());
    }
    rtc_error_ring_push(msg.c_str());
    daily_log_note_sd_error();
}

// Return the current SD card diagnostic log and clear it from RTC
inline String fetch_and_clear_sd_errors() {
    String errs = String(rtc_sd_error_log);
    rtc_sd_error_log[0] = '\0'; // Limpiar vaciando el primer caracter
    return errs;
}


// ==============================================================================
// DEBUG SENSOR LOGIC
// ==============================================================================
extern char rtc_sensor_error_log[256];

inline void add_sensor_error(String msg) {
    // Evitamos desbordamientos de memoria
    if (strlen(rtc_sensor_error_log) + msg.length() < 255) {
        strcat(rtc_sensor_error_log, msg.c_str());
    }
    rtc_error_ring_push(msg.c_str());
}

inline String fetch_and_clear_sensor_errors() {
    String errs = String(rtc_sensor_error_log);
    rtc_sensor_error_log[0] = '\0'; // Limpiamos la variable
    return errs;
}
// ==============================================================================


// Initialize the watchdog timer
inline void init_watchdog() {
    // Initialize the WDT
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true); // true = Panic enabled (forces a restart on timeout)
    
    // Add the current task (setup/loop) to the watchdog
    esp_task_wdt_add(NULL); 
}

// Feed (reset) the watchdog timer
inline void feed_watchdog() {
    esp_task_wdt_reset();
}

extern unsigned long g_wake_start_ms;

inline bool awake_budget_expired() {
    return (millis() - g_wake_start_ms) >= AWAKE_BUDGET_MS;
}

// How much of want_ms still fits in this wake. Zero once the budget is gone.
inline uint32_t cap_awake_wait_ms(uint32_t want_ms) {
    unsigned long elapsed = millis() - g_wake_start_ms;
    return cap_wait_ms(want_ms, (uint32_t)elapsed, (uint32_t)AWAKE_BUDGET_MS);
}

// True when this phase, or the whole wake, has used its deadline.
inline bool phase_deadline_hit(unsigned long phase_start, unsigned long budget_ms) {
    feed_watchdog();
    if ((millis() - phase_start) >= budget_ms) return true;
    if (awake_budget_expired()) return true;
    return false;
}

//Turn on the led for N milli seconds and then turn off the led
inline void blink_led(int red, int green, int blue, int milli_sec=1000) {
    feed_watchdog();
    analogWrite(LED_RED_PIN, red);
    analogWrite(LED_GREEN_PIN, green);
    analogWrite(LED_BLUE_PIN, blue);
    delay(milli_sec/2);
    analogWrite(LED_RED_PIN, 0);
    analogWrite(LED_GREEN_PIN, 0);
    analogWrite(LED_BLUE_PIN, 0);
    delay(milli_sec/2);
    feed_watchdog();
}


// Ensure the SD card is initialized and ready
inline bool ensure_sd_active() {
    // If already initialized, do nothing
    if (is_sdcard_ok) return true;

    local_logger("Mounting SD card...");
    
    // Init SPI with Lilygo Pins
    try {
        SD.end(); // Clean up any previous SD state (important for recovery attempts)
        SPI.end(); // Restart SPI to reset any previous state
        delay(300); // Wait a bit for hardware to settle
        SPI.begin(BOARD_SCK_PIN, BOARD_MISO_PIN, BOARD_MOSI_PIN);
        for(int i=0; i<3; i++){
            if (SD.begin(BOARD_SD_CS_PIN, SPI)) {            
                is_sdcard_ok = true;
                local_logger("SD card mounted.");
                return true;
            }
        }
    } catch(const std::exception& e) {
        local_logger("Exception during SD card init: ", e.what());
        add_sd_error(String("[ENSURE_SD_EXC: ") + e.what() + "] ");
    }
    return false;
}

// Reset the rain sensor by toggling a GPIO pin
inline void reset_sensor() {
    local_logger("Resetting the rain sensor.");
    pinMode(RESET_RAIN_SENSOR_PIN, OUTPUT);
    digitalWrite(RESET_RAIN_SENSOR_PIN, HIGH);
    delay(5000);
    digitalWrite(RESET_RAIN_SENSOR_PIN, LOW); 
}

// Initialize the relay pin
inline void init_relay(){
    pinMode(RELAY_PIN, OUTPUT);
}

// Turn the sensors on by setting the relay pin high
inline void turn_sensors_on(){
    digitalWrite(RELAY_PIN, HIGH);
}

// Turn the sensors off. The relay alone leaves the 5V rail powered.
// v3.0.0 drives that rail low again before sleep; drop it here so the
// station is not still powering the sensors while it waits.
inline void turn_sensors_off(){
    digitalWrite(RELAY_PIN, LOW);
    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, LOW);
    pinMode(PIN_5V_EN, OUTPUT);
    digitalWrite(PIN_5V_EN, LOW);
}


// Forcefully disconnect GPRS session
inline void force_modem_disconnect(TinyGsm& modem) {
    local_logger("Terminating the network session...");

    // This tells the tower: "I am leaving the data network completely."
    modem.sendAT(GF("+CGATT=0"));
    if (modem.waitResponse(10000L) == 1) {
        local_logger("Network detached.");
    }
}

// ==============================================================================
// RUTINA DE DIAGNÓSTICO Y REPARACIÓN DE SD CARD
// ==============================================================================
inline bool diagnose_and_repair_sd() {
    local_logger("Starting SD card self-test...");
    bool is_healthy = true;
    
    // 1. Prueba de Montaje Inicial
    if (!ensure_sd_active()) {
        add_sd_error("[DIAG: MOUNT_FAIL] ");
        is_healthy = false;
        
        // INTENTO DE REPARACIÓN FÍSICA: Reiniciar el Bus SPI
        local_logger("Resetting the SPI bus...");
        SD.end();
        SPI.end();
        delay(200);
        SPI.begin(BOARD_SCK_PIN, BOARD_MISO_PIN, BOARD_MOSI_PIN);
        
        if (SD.begin(BOARD_SD_CS_PIN, SPI)) {
            add_sd_error("[DIAG: BUS_RECOVERY_OK] ");
            is_healthy = true;
        } else {
            add_sd_error("[DIAG: BUS_RECOVERY_FAIL] ");
            return false; // Hardware totalmente inaccesible
        }
    }

    // 2. Prueba de Lectura/Escritura (R/W Test)
    if (is_healthy) {
        const char* test_file = "/test_rw.txt";
        bool rw_ok = false;
        
        // Intentar escribir
        File file = SD.open(test_file, FILE_WRITE);
        if (file) {
            if (file.print("TEST")) {
                rw_ok = true;
            }
            file.close();
        }

        if (rw_ok) {
            // Limpiar la prueba
            SD.remove(test_file); 
            local_logger("SD diagnostic passed.");
            return true;
        } else {
            add_sd_error("[DIAG: RW_TEST_FAIL] ");
            local_logger("SD diagnostic failed while writing.");
            is_healthy = false;
        }
    }

    // 3. Reparación Lógica ("Soft Format" / Wipe)
    // Llegamos aquí si la SD montó pero no permite escribir. Los archivos pueden estar corruptos.
    if (!is_healthy) {
        local_logger("Trying a logical repair by deleting work files...");
        add_sd_error("[DIAG: ATTEMPT_WIPE] ");
        
        bool w1 = SD.remove(QUEUE_FILE);
        bool w2 = SD.remove(STAGING_FILE);
        bool w3 = SD.remove(LEFTOVERS_FILE);

        if (w1 || w2 || w3) {
            add_sd_error("[DIAG: WIPE_PARTIAL_OK] ");
        } else {
            add_sd_error("[DIAG: WIPE_FAIL_OR_EMPTY] ");
        }
        
        // Volvemos a probar escritura tras el wipe
        File file = SD.open("/test_rw.txt", FILE_WRITE);
        if (file && file.print("TEST2")) {
            file.close();
            SD.remove("/test_rw.txt");
            add_sd_error("[DIAG: RECOVERY_SUCCESS] ");
            local_logger("SD repair succeeded.");
            return true;
        } else {
            add_sd_error("[DIAG: RECOVERY_FAIL] ");
            local_logger("The card needs replacement or a format on a PC.");
            return false;
        }
    }
    
    return is_healthy;
}
// ==============================================================================

#endif
