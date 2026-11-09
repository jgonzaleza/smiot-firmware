# New REST API. HTTP APIs cannot require the X-API-Key header the device sends.

resource "aws_api_gateway_rest_api" "firmware_update_v4" {
  name        = local.name
  description = "v4 device firmware update check"

  endpoint_configuration {
    types = ["REGIONAL"]
  }

  tags = local.tags
}

resource "aws_api_gateway_resource" "device_firmware_config" {
  rest_api_id = aws_api_gateway_rest_api.firmware_update_v4.id
  parent_id   = aws_api_gateway_rest_api.firmware_update_v4.root_resource_id
  path_part   = "device_firmware_config"
}

resource "aws_api_gateway_method" "post" {
  rest_api_id      = aws_api_gateway_rest_api.firmware_update_v4.id
  resource_id      = aws_api_gateway_resource.device_firmware_config.id
  http_method      = "POST"
  authorization    = "NONE"
  api_key_required = true
}

resource "aws_api_gateway_integration" "post" {
  rest_api_id             = aws_api_gateway_rest_api.firmware_update_v4.id
  resource_id             = aws_api_gateway_resource.device_firmware_config.id
  http_method             = aws_api_gateway_method.post.http_method
  integration_http_method = "POST"
  type                    = "AWS_PROXY"
  uri                     = aws_lambda_function.firmware_update_v4.invoke_arn
}

resource "aws_api_gateway_deployment" "pro" {
  rest_api_id = aws_api_gateway_rest_api.firmware_update_v4.id

  triggers = {
    redeployment = sha1(jsonencode({
      method      = aws_api_gateway_method.post.id
      integration = aws_api_gateway_integration.post.uri
      path        = aws_api_gateway_resource.device_firmware_config.path_part
      source      = data.archive_file.lambda.output_base64sha256
    }))
  }

  lifecycle {
    create_before_destroy = true
  }

  depends_on = [aws_api_gateway_integration.post]
}

resource "aws_api_gateway_stage" "pro" {
  rest_api_id   = aws_api_gateway_rest_api.firmware_update_v4.id
  deployment_id = aws_api_gateway_deployment.pro.id
  stage_name    = "pro"
  tags          = local.tags
}

resource "aws_api_gateway_api_key" "device" {
  name    = "${local.name}-device"
  enabled = true
  tags    = local.tags
}

resource "aws_api_gateway_usage_plan" "device" {
  name = local.name

  api_stages {
    api_id = aws_api_gateway_rest_api.firmware_update_v4.id
    stage  = aws_api_gateway_stage.pro.stage_name
  }

  # Daily device checks. No quota, so a retry cannot exhaust the plan.
  throttle_settings {
    rate_limit  = 10
    burst_limit = 20
  }

  tags = local.tags
}

resource "aws_api_gateway_usage_plan_key" "device" {
  key_id        = aws_api_gateway_api_key.device.id
  key_type      = "API_KEY"
  usage_plan_id = aws_api_gateway_usage_plan.device.id
}

resource "aws_lambda_permission" "apigateway" {
  statement_id  = "AllowFirmwareUpdateV4Post"
  action        = "lambda:InvokeFunction"
  function_name = aws_lambda_function.firmware_update_v4.function_name
  principal     = "apigateway.amazonaws.com"
  source_arn    = "${aws_api_gateway_rest_api.firmware_update_v4.execution_arn}/*/POST/device_firmware_config"
}
