# Smart IoT end node

Firmware for the LilyGO T-A7608X-S3. It reads RS485 sensors and publishes them to AWS IoT. The default image is cellular. Wi-Fi is a separate build for one client.

`FIRMWARE_VERSION` in `platformio.ini` is the only version number. The sketch reads that macro.

## Build

```bash
pio run -e endnode_cell
pio run -e endnode_wifi
```

`endnode_cell` is the default. It does not link Wi-Fi. `endnode_wifi` adds `-DFEATURE_WIFI`.

The CPU clock is 80 MHz. The partition table has two 3 MB OTA slots and no SPIFFS.

Host checks for the CSV parser and the pack-voltage policy:

```bash
c++ -std=c++17 test/test_power_and_csv.cpp -Iendnode -o /tmp/firmware-tests && /tmp/firmware-tests
```

## Secrets

Copy `endnode/secrets.example.h` to `endnode/secrets.h`. That file is gitignored. Put the Wi-Fi SSID and password there for the Wi-Fi image, and the API keys when you have them. The example values are empty or `changeme`.

The real client certificate headers live only on the build machine. They are gitignored:

- `endnode/certs/AWSClientCertificate.h`
- `endnode/certs/AWSClientPrivateKey.h`

Restore that pair from the `d7b85258` certificate and key under `Certificados AWS/RS485_GSM_SD_CARD_NODE_V_2_x_x`. Do not generate or overwrite those headers.

A build refuses the headers when they contain `placeholder`, or when the certificate lacks `BEGIN CERTIFICATE`, or when the key lacks `PRIVATE KEY`. The placeholder step in `.github/workflows/firmware.yml` is CI-only. Never run it on a build machine. CI compiles those placeholders with `-DALLOW_PLACEHOLDER_CERTS`. That image prints a boot warning and does not upload certificates to the modem.

The Amazon root CA and the firmware signing **public** key are in the repository. Do not commit a private key.

## Variants

| Env | Who | OTA `variant` |
| --- | --- | --- |
| `endnode_cell` | The fleet | `cell` |
| `endnode_wifi` | One client, Wi-Fi with cellular fallback | `wifi` |

`DEVICE_TYPE` is not a build flag. The device reads it from the backend config (2 = weather station, 3 = end node) and stores it in Preferences. The compiled default is 2.

An OTA image whose variant does not match the running build is rejected. A version that is not newer is rejected.

## OTA

The device rejects an update that does not include `S3_CONFIG.SHA256` (lowercase hex of the image) and `S3_CONFIG.SIGNATURE` (base64 DER ECDSA P-256 over those bytes). It checks both before `Update.end()`.

Sign an image with a P-256 key that stays outside Git. The script needs the `cryptography` package (`python3 -m pip install cryptography`):

```bash
openssl ecparam -name prime256v1 -genkey -noout -out firmware-signing-key.pem
python3 scripts/sign_firmware.py --key firmware-signing-key.pem --image .pio/build/endnode_cell/firmware.bin
```

Put the matching public key in `endnode/certs/FirmwareUpdatePublicKey.h`. The private key must not be committed. Copy the printed `SHA256` and `SIGNATURE` into the OTA config.

The OTA URL uses the `prod` stage and falls back to `dev` when the first GET is not 200.

Flash encryption is a written procedure in `docs/flash-encryption-trial.md`. Do not run it from this tree.
