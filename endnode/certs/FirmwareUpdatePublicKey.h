#pragma once
#include <pgmspace.h>

// Public half of an ECDSA P-256 key. The private key is not in this repository.
// Replace this header when the production signing key is created.
static const char FirmwareUpdatePublicKey[] PROGMEM = R"EOF(
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEkYnQ9LntLXXZAzfZHEedYxEQf64q
r/23dOytfqJvGho6milvR8bmilJ+iJ3irGT3Nv5CJK9Sy8cY9GIiMOj0cQ==
-----END PUBLIC KEY-----
)EOF";
