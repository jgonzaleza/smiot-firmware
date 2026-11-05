"""v4 firmware-update check.

One GetItem on DEVICE_FIRMWARE_CONFIG, keyed only by DEVICE_ID.
This function does not write the item and does not use the old availability flag.
"""

import base64
import json
import os


def _as_int(value):
    if value is None or isinstance(value, bool):
        return None
    try:
        number = float(str(value).strip())
    except (TypeError, ValueError):
        return None
    if not number.is_integer():
        return None
    return int(number)


def _device_id(value):
    if value is None or isinstance(value, bool):
        return ""
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if not value.is_integer():
            return ""
        return str(int(value))
    return str(value).strip()


def _text(value):
    if value is None or isinstance(value, (dict, list, bool)):
        return ""
    return str(value).strip()


def _first_text(*values):
    for value in values:
        text = _text(value)
        if text:
            return text
    return ""


def _first_version(*values):
    for value in values:
        number = _as_int(value)
        if number is not None:
            return number
    return None


def _map(value):
    return value if isinstance(value, dict) else {}


def build_response(item, posted_version):
    """Return the device JSON. UPDATE_AVAILABLE is 1 only for a newer signed offer."""
    row = item if isinstance(item, dict) else {}
    stored = _map(row.get("S3_CONFIG"))
    offered = _first_version(
        row.get("LAST_FIRMWARE_VERSION"),
        row.get("FIRMWARE_VERSION"),
        stored.get("VERSION"),
    )
    sha256 = _first_text(stored.get("SHA256"), row.get("SHA256"))
    signature = _first_text(stored.get("SIGNATURE"), row.get("SIGNATURE"))
    newer = (
        offered is not None
        and posted_version is not None
        and offered > posted_version
    )
    signed = bool(sha256) and bool(signature)
    return {
        "UPDATE_AVAILABLE": 1 if newer and signed else 0,
        "BUCKET_NAME": _first_text(stored.get("BUCKET_NAME"), row.get("BUCKET_NAME")),
        "FILE_NAME": _first_text(stored.get("FILE_NAME"), row.get("FILE_NAME")),
        "FIRMWARE_VERSION": offered if offered is not None else 0,
        "VARIANT": _first_text(row.get("VARIANT"), stored.get("VARIANT")),
        "S3_CONFIG": {
            "SHA256": sha256,
            "SIGNATURE": signature,
        },
    }


def _payload(event):
    if not isinstance(event, dict):
        return {}
    raw = event.get("body")
    if raw is None:
        return {}
    if event.get("isBase64Encoded") and isinstance(raw, str):
        try:
            raw = base64.b64decode(raw).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            return {}
    if isinstance(raw, dict):
        return raw
    if not isinstance(raw, str) or not raw.strip():
        return {}
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        return {}
    return parsed if isinstance(parsed, dict) else {}


def _default_table():
    import boto3

    table_name = os.environ.get("DYNAMODB_TABLE", "DEVICE_FIRMWARE_CONFIG")
    return boto3.resource("dynamodb").Table(table_name)


def _load_item(device_id, table):
    if not device_id:
        return None
    found = table.get_item(Key={"DEVICE_ID": device_id})
    item = found.get("Item") if isinstance(found, dict) else None
    return item if isinstance(item, dict) else None


def lambda_handler(event, context, table=None):
    payload = _payload(event)
    device_id = _device_id(payload.get("device_id"))
    posted = _as_int(payload.get("firmware_version"))
    item = None
    if device_id:
        try:
            item = _load_item(device_id, table if table is not None else _default_table())
        except Exception as exc:
            print(f"dynamodb_error={type(exc).__name__}")
            item = None
    body = build_response(item, posted)
    # Device id and the flag only. The event carries the API key, and the row may carry a signature.
    print(f"device_id={device_id} update_available={body['UPDATE_AVAILABLE']}")
    return {
        "statusCode": 200,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(body),
    }
