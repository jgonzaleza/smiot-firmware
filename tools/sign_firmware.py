#!/usr/bin/env python3
"""Sign an end-node image with ECDSA P-256.

The private key stays outside the repository. The device image contains only
the matching public key.

Usage:
  openssl ecparam -name prime256v1 -genkey -noout -out firmware-signing-key.pem
  python3 tools/sign_firmware.py --key firmware-signing-key.pem --image firmware.bin
"""

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Hash and sign a firmware image")
    parser.add_argument("--key", required=True, help="ECDSA P-256 private key PEM, not committed")
    parser.add_argument("--image", required=True, help="Firmware .bin to sign")
    args = parser.parse_args()
    image = Path(args.image)
    key = Path(args.key)
    if not image.is_file():
        print("image not found", file=sys.stderr)
        return 1
    if not key.is_file():
        print("private key not found", file=sys.stderr)
        return 1
    data = image.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    signature = subprocess.check_output(
        ["openssl", "dgst", "-sha256", "-sign", str(key)],
        input=data,
    )
    sig_path = image.with_suffix(image.suffix + ".sig")
    sig_path.write_bytes(signature)
    print("sha256", digest)
    print("signature_file", sig_path)
    print("signature_base64", __import__("base64").b64encode(signature).decode())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
