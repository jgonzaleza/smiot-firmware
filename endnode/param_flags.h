#pragma once

// Command flags from the backend. ArduinoJson 7's as<bool>() is true only for
// JSON true, but the broker sends 1 and 0. The host test passes the same
// shape without linking ArduinoJson.

// present is false when the key is missing or JSON null.
// A bool uses bool_value. A number is on when it is not zero.
// Strings, objects, and arrays are off.
inline bool flag_is_on(bool present, bool is_bool, bool bool_value, bool is_number, long number) {
    if (!present) return false;
    if (is_bool) return bool_value;
    if (is_number) return number != 0;
    return false;
}

// The rain relay pulses on the rising edge: this command is on, and the last
// accepted command was not. A repeated 1 does not pulse until a 0 is accepted.
inline bool rain_pulse_due(bool command_on, bool previous_on) {
    return command_on && !previous_on;
}
