#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

// Uncomment this line to enable logging, or define it in your platformio.ini
//#define DEBUG 

// 1. Base case: Stops recursion when only one argument is left
template <typename T>
void local_logger(T last) {
    #ifdef DEBUG
        Serial.println(last);
    #endif
}

// 2. Recursive case: Handles multiple arguments
template <typename T, typename... Args>
void local_logger(T first, Args... args) {
    #ifdef DEBUG
        Serial.print(first);
        Serial.print(" "); // Add a space between parameters
        local_logger(args...);
    #endif
}

#endif