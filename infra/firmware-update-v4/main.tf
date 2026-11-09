# Existing objects. This stack reads them and never manages them.
# The current firmware API and its two Lambdas are not referenced here.

data "aws_dynamodb_table" "device_firmware_config" {
  name = "DEVICE_FIRMWARE_CONFIG"
}

data "aws_s3_bucket" "firmware_update" {
  bucket = "smartiot-firmware-update"
}

locals {
  name = "firmware-update-v4"
  tags = {
    Name    = "firmware-update-v4"
    Service = "firmware-update"
  }
}
