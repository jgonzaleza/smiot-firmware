#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <Arduino.h>
#include "tinygsm_yield.h"
#include <TinyGsmClient.h>
#include <time.h>
#include <sys/time.h>

// NTP Server Settings
// Array of servers to try in order
const char* ntp_servers[] = {
    "pool.ntp.org",       // 1. Community Cluster (Best general)
    "time.google.com",    // 2. Tech Giant (High availability)
    "time.nist.gov",      // 3. US Govt (Scientific standard)
    "time.cloudflare.com" // 4. Cloudflare (Fast backup)
};

const int current_year = 2026; // Adjust this as years go by

bool time_is_valid = false;

// Time Source Enumeration
enum TimeSource {
    TIME_NOT_SET = 0,   // Year 1970 (Power Loss)
    TIME_RTC_DRIFT = 1, // Woke up, using old time (Medium Confidence)
    TIME_MODEM = 2,     // Synced with Cell Tower (Medium Confidence)
    TIME_NETWORK = 3,   // Synced with Network Provided Time (High Confidence)
    TIME_NTP = 4        // Synced with Atomic Clock (Highest Confidence)    
};

TimeSource current_time_source = TIME_NOT_SET;

// Variable stored in RTC memory (Survives Sleep)
RTC_DATA_ATTR unsigned long last_sync_timestamp = 0;
// Config: Sync every 3600 seconds (1 Hour)
const unsigned long SYNC_INTERVAL = 3600;

// Helper: Check if a timestamp is "sane" (e.g., after Jan 1, 2026)
// Adjust this year as your project ages
bool is_time_sane(struct tm * timeinfo) {
    if (timeinfo->tm_year + 1900 < current_year) return false;
    return true;
}

// Helper function to validate and set the ESP32 system time
bool apply_time_to_esp32(int year, int month, int day, int hour, int min, int sec) {
    // 1. Validate Year (Filter out factory defaults)
    // TinyGSM might return year as "26" or "2026". Normalize to full year.
    int full_year = (year < 100) ? year + 2000 : year;

    if (full_year < current_year) {
        // Reject dates like 1970, 1980, 2004, 2014, etc.
        return false; 
    }

    // 2. Set ESP32 Internal Clock
    struct tm t = {0};
    t.tm_year = full_year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = min;
    t.tm_sec  = sec;
    
    time_t time_val = mktime(&t);
    struct timeval now = { .tv_sec = time_val };
    settimeofday(&now, NULL);
    
    Serial.printf("Time Updated: %04d-%02d-%02d %02d:%02d:%02d\n", 
                  full_year, month, day, hour, min, sec);
    return true;
}

// ---------------------------------------------------------
// STRATEGY 1: NTP (Network Time Protocol)
// ---------------------------------------------------------
// Helper: Tries a SINGLE specific server
bool try_modem_ntp(TinyGsm& modem, const char* server) {

local_logger("Configuring Modem NTP:", server);

    // 1. SETUP: AT+CNTP="host",0
    // We set timezone to 0 (UTC) as discussed
    String cmd = "+CNTP=\"";
    cmd += server;
    cmd += "\",0"; 
    
    modem.sendAT(GF(cmd));
    if (modem.waitResponse() != 1) {
        local_logger("NTP Config Failed");
        return false;
    }

    // 2. EXECUTE: AT+CNTP
    modem.sendAT(GF("+CNTP"));
    
    // STEP A: Wait for immediate "OK" confirmation
    // This confirms the modem started the task.
    if (modem.waitResponse() != 1) {
        local_logger("NTP Command Rejected");
        return false;
    }

    // STEP B: Wait for the URC (Unsolicited Result Code)
    // This confirms the task actually FINISHED.
    // The manual says: +CNTP: <err>
    // We give it 20 seconds because network can be slow.
    // 1 = Match "+CNTP: 0" (Success)
    // 2 = Match "+CNTP: 1" (Network Timeout)
    // 3 = Match "+CNTP: 61" (DNS Error)
    
    local_logger("Waiting for NTP Result...");
    int res = modem.waitResponse(20000L, "+CNTP: 0", "+CNTP: 1", "+CNTP: 61");

    if (res == 1) {
        local_logger("NTP Success (Refreshed)!");
        return true;
    } else if (res == 2) {
        local_logger("NTP Fail: Network Timeout");
    } else if (res == 3) {
        local_logger("NTP Fail: DNS Error");
    } else {
        local_logger("NTP Fail: Unknown Error or Timeout");
    }

    return false;
}

// Full NTP Sync Function
bool sync_via_ntp(TinyGsm& modem) {
    // Loop through all 4 servers
    for (int i = 0; i < 4; i++) {
        if (try_modem_ntp(modem, ntp_servers[i])) {
            
            // --- SUCCESS ---
            // The modem has the time internally. Now read it out.
            delay(1000); // Wait a moment for registers to update
            
            int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
            float tz = 0;
            
            if (modem.getNetworkTime(&year, &month, &day, &hour, &min, &sec, &tz)) {
                if (apply_time_to_esp32(year, month, day, hour, min, sec)) {                    
                    local_logger("Time Synced via:", ntp_servers[i]);
                    return true;
                }
            }
        }
        // If we are here, that server failed. Loop continues to the next one.
        local_logger("Server failed. Trying next...");
        delay(1000);
    }
    
    local_logger("Critical: All NTP Servers failed.");
    return false;
}



// ---------------------------------------------------------
// STRATEGY 2: Network Time (NITZ / Location Services)
// ---------------------------------------------------------
bool sync_via_network(TinyGsm& modem) {
    bool return_value = false;
    // This is the new method you found.
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    float tz = 0;

    local_logger("Trying getNetworkTime...");
    if (modem.getNetworkTime(&year, &month, &day, &hour, &min, &sec, &tz)) {
        if (apply_time_to_esp32(year, month, day, hour, min, sec)) {
            local_logger("Source: getNetworkTime (Success)");
            return_value = true;
        }
    }
    return return_value;
}

// ---------------------------------------------------------
// STRATEGY 3: Modem Network Time (CCLK)
// ---------------------------------------------------------
bool sync_via_modem(TinyGsm& modem) {
    bool return_value = false;
    // Request time from modem
    local_logger("Starting Modem time sync...");
    String timeStr = modem.getGSMDateTime(DATE_FULL); 
    // Format expected: "26/01/27,14:22:55+32" (YY/MM/DD,hh:mm:ss+zz)
    
    // Basic String Validation
    if (timeStr.length() < 17) return false;

    int year = timeStr.substring(0, 2).toInt() + 2000;
    int month = timeStr.substring(3, 5).toInt();
    int day = timeStr.substring(6, 8).toInt();
    int hour = timeStr.substring(9, 11).toInt();
    int min = timeStr.substring(12, 14).toInt();
    int sec = timeStr.substring(15, 17).toInt();

    // Apply to ESP32
    if (apply_time_to_esp32(year, month, day, hour, min, sec)) {
        local_logger("Source: Modem CCLK (Success)");
        return_value = true;
    }
    return return_value;
}

// print a timestamp in human-readable format
void print_date(unsigned long timestamp) {
    #ifdef DEBUG
        local_logger("Converting Timestamp:", timestamp);
        // 1. Convert to standard time data type
        time_t raw_time = (time_t)timestamp;
        
        // 2. Convert to Local Time structure
        // This uses the Timezone set in configTime() (UTC-6 for Costa Rica)
        struct tm * timeinfo = localtime(&raw_time);
        
        // 3. Format into a string (YYYY-MM-DD HH:MM:SS)
        char buffer[30];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
        
        // 4. Print using your logger
        // We pass the string as the second parameter
        local_logger("Converted Date:", buffer);
    #endif
}

// Returns the current Epoch time (Seconds since 1970)
// Returns 0 if time is invalid/unset
unsigned long get_current_timestamp() {
    local_logger("Getting Current Timestamp...");
    time_t now;
    time(&now); // Read ESP32 system clock
    
    // Optional: Check if year is valid (e.g. > 2026)
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year + 1900 < current_year) {
        time_is_valid = false;
        current_time_source = TIME_NOT_SET;
        local_logger("Current Timestamp - Time NOT Valid.");
        return 0; // Time not valid yet
    } else {
        time_is_valid = true;
        if (current_time_source == TIME_NOT_SET) {
            current_time_source = TIME_RTC_DRIFT;
        }        
        local_logger("Current Timestamp - Time Valid:");
        print_date((unsigned long)now);
    }
    
    return (unsigned long)now;
}

// ---------------------------------------------------------
// MAIN SYNC FUNCTION
// ---------------------------------------------------------
// Helper to actually do the network work
void perform_full_sync(TinyGsm& modem) {
    bool synced = false;
    
    // Try NTP first (Most Accurate)
    if (sync_via_ntp(modem)) {
        local_logger("NTP Time Sync Successful");
        synced = true;
        current_time_source = TIME_NTP;
    } else if (sync_via_network(modem)) {
        // Fallback to Network Time
        local_logger("Network Time Sync Successful");
        synced = true;
        current_time_source = TIME_NETWORK;
    } else if (sync_via_modem(modem)) {
        // Fallback to Modem Time
        local_logger("Modem Time Sync Successful");
        synced = true;
        current_time_source = TIME_MODEM;
    }

    int timestamp = get_current_timestamp();

    if (synced && timestamp > 0) {
        local_logger("Time Sync Completed Successfully");
        // Update the timestamp of "Last Good Sync"
        time_t t = time(NULL);
        last_sync_timestamp = (unsigned long)t;
    } else {
        local_logger("Time Sync Failed. Try one more time.");
        if (sync_via_ntp(modem)) {
            local_logger("NTP Time Sync Successful");
            synced = true;
            current_time_source = TIME_NTP;
            time_t t = time(NULL);
            last_sync_timestamp = (unsigned long)t;
        } else {
            local_logger("Final Attempt Failed. Time is NOT valid.");
            time_is_valid = false;
            current_time_source = TIME_NOT_SET;
        }
    }
}

// Main Reliable Time Sync Function
void reliable_time_sync(TinyGsm& modem) {
    // 1. Get Current Internal Time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // 2. CHECK: Is Time Broken? (Year < 2025)
    // If we lost power, we MUST sync immediately.
    if (timeinfo.tm_year + 1900 < current_year) {
        local_logger("Time Invalid (Power Loss). Forcing Sync...");
        perform_full_sync(modem);
        return;
    }

    // 3. CHECK: Has it been 1 hour?
    // 'now' is the current epoch time. 'last_sync_timestamp' is the last time we checked NTP.
    if (now - last_sync_timestamp > SYNC_INTERVAL) {
        local_logger("Time Stale. Re-syncing...\n", (now - last_sync_timestamp));
        perform_full_sync(modem);
    } else {
        // Time is valid and fresh enough. Skip network call.
        local_logger("Time is fresh. Skipping Network Sync.");
        time_is_valid = true;
    }
}

// ---------------------------------------------------------
// RETROACTIVE TIMESTAMP CALCULATOR
// ---------------------------------------------------------
unsigned long get_accurate_timestamp(unsigned long reading_millis) {
    time_t now_sec;
    time(&now_sec);
    
    if (time_is_valid) {
        // We have real time.
        // Calculate how many seconds ago the reading happened.
        unsigned long diff_ms = millis() - reading_millis;
        unsigned long diff_sec = diff_ms / 1000;
        
        // Return the real timestamp of the reading
        return (unsigned long)now_sec - diff_sec;
    } else {
        // We still have NO valid time (very rare).
        // Return 0 or a flag to tell the server "I don't know the date".
        return 0; 
    }
}

#endif