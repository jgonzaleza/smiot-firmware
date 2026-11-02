# Per-device certificates

The fleet currently shares one AWS IoT certificate. A leak of that certificate exposes every device. The replacement is one certificate per device, issued by AWS IoT fleet provisioning.

That provisioning flow is backend work. This repository does not contain AWS account ids, provisioning claim certificates, private keys, or fleet templates.

## Device side

- The modem stores the device certificate and private key once, in its own certificate store (`ca_cert.pem` stays the Amazon root; the client cert and key are the device pair).
- The store entry is keyed by the device MAC from `ESP.getEfuseMac()`, which is already the MQTT client identity.
- `AT+CCERTLIST` is checked before another upload so a reboot does not rewrite the same files.
- Revoking one device is a backend change to that certificate. The shared certificate can be retired after the fleet has moved.

## Backend side

The backend project owns fleet provisioning: the claim certificate, the provisioning template, and writing the new certificate id back to the device record. Do not invent those credentials in this repository.
