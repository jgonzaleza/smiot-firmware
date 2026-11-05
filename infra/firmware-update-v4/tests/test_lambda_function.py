import importlib.util
import json
import unittest
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "lambda_function.py"
SPEC = importlib.util.spec_from_file_location("firmware_update_v4_lambda", SOURCE)
lambda_function = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lambda_function)


def row(**overrides):
    item = {
        "DEVICE_ID": "189603893824884",
        "DEVICE_CLIENT_ID": "A7608-S3-01-005",
        "DEVICE_TYPE": 2,
        "LAST_FIRMWARE_VERSION": 400,
        "VARIANT": "cell",
        "S3_CONFIG": {
            "BUCKET_NAME": "smartiot-firmware-update",
            "FILE_NAME": "endnode_cell_400.bin",
            "SHA256": "ab" * 32,
            "SIGNATURE": "c2ln",
        },
    }
    item.update(overrides)
    return item


class FakeTable:
    def __init__(self, item):
        self.item = item
        self.keys = []
        self.writes = []

    def get_item(self, Key):
        self.keys.append(Key)
        if self.item is None:
            return {}
        return {"Item": self.item}

    def update_item(self, **kwargs):
        self.writes.append(kwargs)
        raise AssertionError("lambda must not write the item")


class DecisionTests(unittest.TestCase):
    def test_newer_signed_offer_is_available(self):
        body = lambda_function.build_response(row(), 300)
        self.assertEqual(
            body,
            {
                "UPDATE_AVAILABLE": 1,
                "BUCKET_NAME": "smartiot-firmware-update",
                "FILE_NAME": "endnode_cell_400.bin",
                "FIRMWARE_VERSION": 400,
                "VARIANT": "cell",
                "S3_CONFIG": {"SHA256": "ab" * 32, "SIGNATURE": "c2ln"},
            },
        )

    def test_same_or_older_offer_is_not_available(self):
        self.assertEqual(lambda_function.build_response(row(), 400)["UPDATE_AVAILABLE"], 0)
        self.assertEqual(lambda_function.build_response(row(), 401)["UPDATE_AVAILABLE"], 0)
        same = lambda_function.build_response(row(), 400)
        self.assertEqual(same["FIRMWARE_VERSION"], 400)
        self.assertEqual(same["S3_CONFIG"]["SHA256"], "ab" * 32)

    def test_missing_signature_fields_block_the_offer(self):
        missing_sha = row()
        del missing_sha["S3_CONFIG"]["SHA256"]
        missing_sig = row()
        missing_sig["S3_CONFIG"]["SIGNATURE"] = "  "
        self.assertEqual(lambda_function.build_response(missing_sha, 300)["UPDATE_AVAILABLE"], 0)
        self.assertEqual(lambda_function.build_response(missing_sig, 300)["UPDATE_AVAILABLE"], 0)

    def test_available_flag_is_ignored(self):
        item = row(FIRMWARE_AVAILABLE=True, LAST_FIRMWARE_VERSION=300)
        body = lambda_function.build_response(item, 300)
        self.assertEqual(body["UPDATE_AVAILABLE"], 0)
        self.assertNotIn("FIRMWARE_AVAILABLE", body)

    def test_version_falls_through_last_then_root_then_s3(self):
        self.assertEqual(lambda_function.build_response(row(FIRMWARE_VERSION=500), 300)["FIRMWARE_VERSION"], 400)
        without_last = row()
        del without_last["LAST_FIRMWARE_VERSION"]
        without_last["FIRMWARE_VERSION"] = 410
        without_last["S3_CONFIG"]["VERSION"] = 420
        self.assertEqual(lambda_function.build_response(without_last, 300)["FIRMWARE_VERSION"], 410)
        only_s3 = row()
        del only_s3["LAST_FIRMWARE_VERSION"]
        only_s3["S3_CONFIG"]["VERSION"] = Decimal("430")
        self.assertEqual(lambda_function.build_response(only_s3, 300)["FIRMWARE_VERSION"], 430)

    def test_variant_and_signature_prefer_the_stored_object(self):
        item = row()
        item["S3_CONFIG"]["VARIANT"] = "wifi"
        item["SHA256"] = "ignored"
        body = lambda_function.build_response(item, 300)
        self.assertEqual(body["VARIANT"], "cell")
        self.assertEqual(body["S3_CONFIG"]["SHA256"], "ab" * 32)
        no_root_variant = row()
        del no_root_variant["VARIANT"]
        no_root_variant["S3_CONFIG"]["VARIANT"] = "wifi"
        self.assertEqual(lambda_function.build_response(no_root_variant, 300)["VARIANT"], "wifi")

    def test_missing_row_returns_an_empty_offer(self):
        self.assertEqual(
            lambda_function.build_response(None, 400),
            {
                "UPDATE_AVAILABLE": 0,
                "BUCKET_NAME": "",
                "FILE_NAME": "",
                "FIRMWARE_VERSION": 0,
                "VARIANT": "",
                "S3_CONFIG": {"SHA256": "", "SIGNATURE": ""},
            },
        )

    def test_source_does_not_write_or_read_the_old_flag(self):
        text = SOURCE.read_text()
        for banned in ("update_item", "put_item", "delete_item", "batch_write", "FIRMWARE_AVAILABLE"):
            self.assertNotIn(banned, text)


class HandlerTests(unittest.TestCase):
    def _event(self, payload, **extra):
        event = {"body": json.dumps(payload)}
        event.update(extra)
        return event

    def test_lookup_uses_device_id_only_and_does_not_write(self):
        table = FakeTable(row())
        event = self._event(
            {
                "client_id": "someone-else-entirely",
                "device_type": 9,
                "variant": "wifi",
                "firmware_version": 300,
                "device_id": "189603893824884",
            },
            headers={"x-api-key": "do-not-log"},
        )
        result = lambda_function.lambda_handler(event, None, table=table)
        self.assertEqual(table.keys, [{"DEVICE_ID": "189603893824884"}])
        self.assertEqual(table.writes, [])
        self.assertEqual(result["statusCode"], 200)
        self.assertEqual(json.loads(result["body"])["UPDATE_AVAILABLE"], 1)

    def test_numeric_device_id_is_the_string_key(self):
        table = FakeTable(None)
        result = lambda_function.lambda_handler(
            self._event({"device_id": 88259588205692, "firmware_version": 300}),
            None,
            table=table,
        )
        self.assertEqual(table.keys, [{"DEVICE_ID": "88259588205692"}])
        self.assertEqual(json.loads(result["body"])["UPDATE_AVAILABLE"], 0)

    def test_missing_device_id_skips_dynamodb(self):
        table = FakeTable(row())
        result = lambda_function.lambda_handler(
            self._event({"firmware_version": 300, "client_id": "A7608-S3-01-005"}),
            None,
            table=table,
        )
        self.assertEqual(table.keys, [])
        self.assertEqual(json.loads(result["body"])["UPDATE_AVAILABLE"], 0)

    def test_invalid_json_is_http_200_with_no_offer(self):
        table = FakeTable(row())
        result = lambda_function.lambda_handler({"body": '{"variant":"cell,'}, None, table=table)
        self.assertEqual(result["statusCode"], 200)
        self.assertEqual(table.keys, [])
        self.assertEqual(json.loads(result["body"])["UPDATE_AVAILABLE"], 0)

    def test_posted_version_string_and_decimal_row(self):
        item = row(LAST_FIRMWARE_VERSION=Decimal("400"))
        table = FakeTable(item)
        result = lambda_function.lambda_handler(
            self._event({"device_id": "189603893824884", "firmware_version": "300"}),
            None,
            table=table,
        )
        body = json.loads(result["body"])
        self.assertEqual(body["UPDATE_AVAILABLE"], 1)
        self.assertEqual(body["FIRMWARE_VERSION"], 400)


if __name__ == "__main__":
    unittest.main()
