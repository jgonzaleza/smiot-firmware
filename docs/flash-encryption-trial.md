# Flash encryption trial

This is a written procedure only. Do not run it as part of a firmware build, and do not burn eFuses from this repository.

Use development mode, on one spare LilyGO T-A7608X-S3 that can be lost. Release mode burns eFuses permanently and is out of scope until OTA, NVS and the SD card have been checked on that spare board.

## Before you start

- The board is a spare. The fleet keeps running the unsigned development images.
- You have a USB cable, esptool.py from PlatformIO, and a copy of the `endnode_cell` image.
- You have saved the esptool output from every command below.
- Flash encryption is in development mode (`SPI_BOOT_CRYPT_CNT` is not yet write-protected).

## Steps

1. Confirm the current mode. Encryption must not already be enabled:

   ```bash
   python3 -m esptool --chip esp32s3 flash_id
   python3 -m esptool --chip esp32s3 get_security_info
   ```

   Stop if `get_security_info` reports flash encryption already enabled or if the board is not the spare.

2. Build the cellular image only:

   ```bash
   pio run -e endnode_cell
   ```

3. Flash the image in development mode. Do not pass `--flash_crypt_conf` or any release-mode option. Do not use `espefuse.py burn_efuse`.

   ```bash
   python3 -m esptool --chip esp32s3 --before default_reset --after no_reset write_flash 0x0 .pio/build/endnode_cell/bootloader.bin
   ```

   The first boot in development mode encrypts the flash and reboots. Save the serial log.

4. On that spare board, check:

   - the device still publishes one MQTT message
   - an OTA of a newer cellular image still boots
   - Preferences (NVS) still return the saved sleep interval after the OTA reboot
   - the SD queue file is still readable

5. Leave the board in development mode. Do not burn `SPI_BOOT_CRYPT_CNT` to its final value, and do not set the write-protect bits.

Per-device certificates protect the fleet more than flash encryption does, because one dumped key no longer opens every device. Finish that move before anyone considers release mode.
