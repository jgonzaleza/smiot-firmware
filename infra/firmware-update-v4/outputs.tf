output "invoke_url" {
  description = "POST URL for a v4 device."
  value       = "${aws_api_gateway_stage.pro.invoke_url}/device_firmware_config"
}

output "api_key_id" {
  description = "ID of the generated device API key."
  value       = aws_api_gateway_api_key.device.id
}

output "api_key_value" {
  description = "X-API-Key for v4 devices. Sensitive. Read it only after apply, and do not commit it."
  value       = aws_api_gateway_api_key.device.value
  sensitive   = true
}

output "lambda_function_name" {
  value = aws_lambda_function.firmware_update_v4.function_name
}

output "firmware_config_table_arn" {
  description = "Existing table. This stack does not manage it."
  value       = data.aws_dynamodb_table.device_firmware_config.arn
}

output "firmware_bucket_arn" {
  description = "Existing firmware bucket. This stack does not manage it and the Lambda has no S3 permissions."
  value       = data.aws_s3_bucket.firmware_update.arn
}
