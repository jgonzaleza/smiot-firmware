# Per-device certificates

The fleet currently shares one AWS IoT certificate. A leak of that certificate exposes every device.

The chosen replacement is AWS IoT fleet provisioning by claim. That work is deferred to the next iteration. This repository does not implement it, and it does not contain AWS account ids, claim certificates, private keys, or provisioning templates.

## Next iteration

The backend project owns the claim certificate, the provisioning template, and the pre-provisioning Lambda.

The firmware will:

- Connect with the claim certificate.
- Call `CreateKeysAndCertificate` and `RegisterThing`.
- Store the device certificate in NVS and in the modem.

Until that lands, devices keep using the shared certificate. Do not add claim material or a provisioning flow to this tree.
