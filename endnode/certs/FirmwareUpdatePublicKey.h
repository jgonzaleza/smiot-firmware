#pragma once
#include <pgmspace.h>

// Production ECDSA P-256 public key. The private key is not in this repository.
static const char FirmwareUpdatePublicKey[] PROGMEM = R"EOF(
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEkYnQ9LntLXXZAzfZHEedYxEQf64q
r/23dOytfqJvGho6milvR8bmilJ+iJ3irGT3Nv5CJK9Sy8cY9GIiMOj0cQ==
-----END PUBLIC KEY-----
)EOF";
