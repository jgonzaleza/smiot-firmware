#pragma once
#include <pgmspace.h>

// Public half of an ECDSA P-256 key. The private key is not in this repository.
// Replace this header when the production signing key is created.
static const char FirmwareUpdatePublicKey[] PROGMEM = R"EOF(
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEKTudGKH7xm79Dov8XyEpvSWsD8XX
4KYqNO46bV1YGGXlRonnveHk2gTYBptoFDrUuoNH8+IY1zkkpxhir6LDKA==
-----END PUBLIC KEY-----
)EOF";
