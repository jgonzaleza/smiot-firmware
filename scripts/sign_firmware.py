#!/usr/bin/env python3
"""Print the SHA-256 and base64 ECDSA P-256 signature of a firmware image.

The device rejects an OTA image unless the config document includes:

    S3_CONFIG.SHA256      lowercase hex SHA-256 of the raw image bytes
    S3_CONFIG.SIGNATURE   standard base64 of the DER ECDSA P-256 signature
                          over those same bytes (hash algorithm SHA-256)

Upload those two fields with the image. The private key is an argument and
must not be committed.

This script prefers the cryptography library, which is not part of the
stdlib:

    python3 -m pip install cryptography

Usage:
    python3 scripts/sign_firmware.py --key firmware-signing-key.pem --image firmware.bin
"""

import argparse
import base64
import hashlib
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Hash and sign a firmware image")
    parser.add_argument("--key", required=True, help="ECDSA P-256 private key PEM, not committed")
    parser.add_argument("--image", required=True, help="Firmware .bin to sign")
    args = parser.parse_args()
    image = Path(args.image)
    key_path = Path(args.key)
    if not image.is_file():
        print("image not found", file=sys.stderr)
        return 1
    if not key_path.is_file():
        print("private key not found", file=sys.stderr)
        return 1
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError:
        print("cryptography is required: python3 -m pip install cryptography", file=sys.stderr)
        return 1

    data = image.read_bytes()
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
        print("key must be an ECDSA P-256 private key", file=sys.stderr)
        return 1
    signature = key.sign(data, ec.ECDSA(hashes.SHA256()))
    digest = hashlib.sha256(data).hexdigest()
    print("SHA256", digest)
    print("SIGNATURE", base64.b64encode(signature).decode("ascii"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
