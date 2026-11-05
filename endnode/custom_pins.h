// LilyGO T-A7608X-S3 GPIOs. The T-SimHat silkscreen uses the original ESP32 names;
// the S3 header maps them by position:
//   hat IO19 -> GPIO41 RS485 TX
//   hat IO18 -> GPIO40 RS485 RX
//   hat IO23 -> GPIO42 RS485 DE
//   hat IO32 -> GPIO5  sensor relay
//   hat IO0  -> GPIO0  hat 5V enable
#pragma once

#define PIN_5V_EN    0
#define RS485_TX_PIN 41
#define RS485_RX_PIN 40
#define RS485_EN_PIN 42
#define RELAY_PIN 5

#define LED_RED_PIN 8
#define LED_GREEN_PIN 45
#define LED_BLUE_PIN 48

#define RESET_RAIN_SENSOR_PIN 46
