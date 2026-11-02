#pragma once

// Feed the task watchdog during TinyGSM AT waits. Defined before TinyGsmCommon.h
// so the default delay(0) yield is replaced. Safe to include more than once.
#if !defined(TINY_GSM_YIELD)
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <esp_task_wdt.h>
#define TINY_GSM_YIELD() \
  do {                   \
    esp_task_wdt_reset();\
    delay(0);            \
  } while (0)
#endif
#endif
