# TinyGSM vendor record

| Field | Value |
| --- | --- |
| Version | 1.0.0 (`library.json`) |
| Source | LilyGO [LilyGo-Modem-Series](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series) |
| Commit | `d6165e3d427e8a4603b9d0999ac929b7d8412824` (short `d6165e3`) |
| Path in that tree | `lib/TinyGSM` |
| Taken from | `https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/archive/d6165e3.tar.gz` |

## Local edits

`patches/v3.4.0-on-1.0.0.patch` is the original port onto that commit. These are the local edits in the tree now:

- `TinyGsmHttpsComm.h`: HTTPS `sslversion` and SNI are written to SSL context 1, not context 0. `https_get_size()` waits 20 seconds instead of 120.
- `TinyGsmMqttA76xx.h`: `AT+CCERTLIST` runs before `AT+CCERTDOWN`, and a reload deletes the three modem files first. Each PEM must start with `-----BEGIN` and have a plausible length; anything else is refused. The `+CMQTTCONNECT` result is the full code. MQTT context 0 uses `sslversion` 3 (TLS 1.2). When the client certificate and key are both present, `authmode` stays 2.
- `TinyGsmClientA7608.h`: `enableNetwork()` and `disableNetwork()` are restored. 1.0.0 replaced them with `setNetworkActive()`, which closes the PDP before every open.

The N/S and E/W GPS sign is already applied in this commit (`TinyGsmClientA76xx.h` `getGPSImpl`). That older local edit is not reapplied.

The firmware still configures HTTPS context 1 (`cacert`, `authmode` 1, `https_set_ssl_index(1)`) in `CellularCommunicationClient` and passes `TINYGSM_SSL_TLS1_2` into `https_set_url`.

## Device certificates

The real `endnode/certs/AWSClientCertificate.h` and `endnode/certs/AWSClientPrivateKey.h` live only on the build machine. Restore them from the `d7b85258` certificate pair under `Certificados AWS/RS485_GSM_SD_CARD_NODE_V_2_x_x`. Do not generate or overwrite those headers. The placeholder step in `.github/workflows/firmware.yml` is CI-only and must never be run on a build machine.
