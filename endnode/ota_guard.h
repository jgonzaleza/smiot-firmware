#pragma once

#include <stddef.h>
#include <stdint.h>

void ota_remember_slot();
void ota_note_boot();
void ota_confirm_image();

// ECDSA P-256 over the SHA-256 of the image. signature_b64 is standard base64 of the DER signature.
// False when the signature is missing, malformed, or does not match FirmwareUpdatePublicKey.
bool ota_signature_valid(const uint8_t hash[32], const char* signature_b64);
