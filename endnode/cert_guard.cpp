#include "cert_guard.h"

#include <Arduino.h>

void cert_guard_boot_warning() {
#ifdef ALLOW_PLACEHOLDER_CERTS
    // firmware_context.h compiles Serial out unless DEBUG is set. This file
    // does not include that header, so the warning still reaches the UART.
    Serial.begin(115200);
    Serial.println();
    Serial.println("****************************************************************");
    Serial.println("ALLOW_PLACEHOLDER_CERTS");
    Serial.println("This image contains placeholder AWS certificates.");
    Serial.println("It will not upload certificates to the modem.");
    Serial.println("Do not flash this image onto a device.");
    Serial.println("****************************************************************");
    Serial.flush();
#endif
}
