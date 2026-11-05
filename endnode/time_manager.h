#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <Arduino.h>
#include "tinygsm_yield.h"
#include <TinyGsmClient.h>
#include <time.h>
#include <sys/time.h>

extern const char* ntp_servers[4];
static const int current_year = 2026;

extern bool time_is_valid;

enum TimeSource {
    TIME_NOT_SET = 0,
    TIME_RTC_DRIFT = 1,
    TIME_MODEM = 2,
    TIME_NETWORK = 3,
    TIME_NTP = 4
};

extern TimeSource current_time_source;
extern unsigned long last_sync_timestamp;
static const unsigned long SYNC_INTERVAL = 3600;

bool is_time_sane(struct tm* timeinfo);
bool apply_time_to_esp32(int year, int month, int day, int hour, int min, int sec);
bool try_modem_ntp(TinyGsm& modem, const char* server);
bool sync_via_ntp(TinyGsm& modem);
bool sync_via_network(TinyGsm& modem);
bool sync_via_modem(TinyGsm& modem);
void print_date(unsigned long timestamp);
unsigned long get_current_timestamp();
void perform_full_sync(TinyGsm& modem);
void reliable_time_sync(TinyGsm& modem);
unsigned long get_accurate_timestamp(unsigned long reading_millis);
#ifdef FEATURE_WIFI
void reliable_time_sync_wifi();
#endif

#endif
