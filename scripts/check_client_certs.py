#!/usr/bin/env python3
"""Pre-build check for the gitignored AWS client certificate headers.

Fails when endnode/certs/AWSClient*.h contains "placeholder" or lacks
"BEGIN CERTIFICATE" / "PRIVATE KEY". -DALLOW_PLACEHOLDER_CERTS bypasses
the check for the CI compile. The script never prints header contents.
"""

import re
import sys
import tempfile
import shutil
from pathlib import Path

REQUIRED = (
    ("AWSClientCertificate.h", "BEGIN CERTIFICATE"),
    ("AWSClientPrivateKey.h", "PRIVATE KEY"),
)


def flags_allow_placeholder(text):
    return "ALLOW_PLACEHOLDER_CERTS" in (text or "")


def evaluate(certs_dir):
    """Return error strings. An empty list means the headers may be compiled."""
    errors = []
    paths = sorted(Path(certs_dir).glob("AWSClient*.h"))
    if not paths:
        errors.append("endnode/certs/AWSClient*.h is missing")
        return errors
    seen = {path.name: path for path in paths}
    required_names = set()
    for name, marker in REQUIRED:
        required_names.add(name)
        path = seen.get(name)
        if path is None:
            errors.append("%s is missing" % name)
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "placeholder" in text:
            errors.append("%s contains placeholder" % name)
        if marker not in text:
            errors.append("%s lacks %s" % (name, marker))
    for path in paths:
        if path.name in required_names:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "placeholder" in text:
            errors.append("%s contains placeholder" % path.name)
    return errors


def run_check(certs_dir, allow, quiet=False):
    if allow:
        if not quiet:
            print("ALLOW_PLACEHOLDER_CERTS: certificate header check bypassed")
        return 0
    errors = evaluate(certs_dir)
    if not errors:
        if not quiet:
            print("certificate headers accepted")
        return 0
    if not quiet:
        for err in errors:
            print("ERROR: %s" % err, file=sys.stderr)
        print("ERROR: refusing to build. Real AWS client headers are required.", file=sys.stderr)
        print("ERROR: CI bypasses this check with -DALLOW_PLACEHOLDER_CERTS.", file=sys.stderr)
    return 1


def _gather_flag_text(env):
    chunks = []
    flags = env.get("BUILD_FLAGS", [])
    if isinstance(flags, (list, tuple)):
        chunks.extend(str(item) for item in flags)
    elif flags:
        chunks.append(str(flags))
    option = ""
    getter = getattr(env, "GetProjectOption", None)
    if getter is not None:
        try:
            option = getter("build_flags", "")
        except Exception:
            option = ""
    if isinstance(option, (list, tuple)):
        chunks.extend(str(item) for item in option)
    elif option:
        chunks.append(str(option))
    defines = env.get("CPPDEFINES", [])
    for item in defines:
        name = item[0] if isinstance(item, (list, tuple)) else item
        chunks.append(str(name))
    return " ".join(chunks)


def effective_flag_text(raw_by_env, env_name):
    """Follow ${env:<name>.build_flags} so an extended env sees the base flags."""
    seen = set()
    chunks = []

    def walk(name):
        if not name or name in seen:
            return
        seen.add(name)
        raw = raw_by_env.get(name, "") or ""
        chunks.append(raw)
        for match in re.findall(r"\$\{env:([^.}]+)\.build_flags\}", raw):
            walk(match)

    walk(env_name)
    return " ".join(chunks)


def allow_from_env(env):
    chunks = [_gather_flag_text(env)]
    raw_by_env = {}
    current = ""
    try:
        current = env["PIOENV"]
    except Exception:
        current = ""
    config = getattr(env, "GetProjectConfig", None)
    project = config() if config is not None else None
    if project is not None:
        try:
            sections = project.sections()
        except Exception:
            sections = []
        for section in sections:
            if not str(section).startswith("env:"):
                continue
            name = str(section)[4:]
            try:
                raw_by_env[name] = project.get(section, "build_flags") or ""
            except Exception:
                raw_by_env[name] = ""
    if current:
        chunks.append(effective_flag_text(raw_by_env, current))
    return flags_allow_placeholder(" ".join(chunks))


def _write_pair(directory, certificate, key):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "AWSClientCertificate.h").write_text(certificate)
    (directory / "AWSClientPrivateKey.h").write_text(key)


def self_test():
    root = Path(tempfile.mkdtemp(prefix="cert-guard-"))
    try:
        bad = root / "placeholder"
        _write_pair(
            bad,
            'static const char AWSClientCertificate[] = "placeholder-certificate\\n";\n',
            'static const char AWSClientPrivateKey[] = "placeholder-private-key\\n";\n',
        )
        if run_check(bad, False, quiet=True) == 0:
            print("ERROR: placeholder headers were accepted", file=sys.stderr)
            return 1
        if run_check(bad, True, quiet=True) != 0:
            print("ERROR: -DALLOW_PLACEHOLDER_CERTS did not bypass the check", file=sys.stderr)
            return 1
        good = root / "pem"
        _write_pair(
            good,
            'static const char AWSClientCertificate[] = "-----BEGIN CERTIFICATE-----\\nMIIB\\n-----END CERTIFICATE-----\\n";\n',
            'static const char AWSClientPrivateKey[] = "-----BEGIN PRIVATE KEY-----\\nMIIB\\n-----END PRIVATE KEY-----\\n";\n',
        )
        if run_check(good, False, quiet=True) != 0:
            print("ERROR: headers with BEGIN CERTIFICATE and PRIVATE KEY were rejected", file=sys.stderr)
            return 1
        bare = root / "bare"
        _write_pair(
            bare,
            'static const char AWSClientCertificate[] = "not-a-certificate";\n',
            'static const char AWSClientPrivateKey[] = "not-a-key";\n',
        )
        if run_check(bare, False, quiet=True) == 0:
            print("ERROR: headers without PEM markers were accepted", file=sys.stderr)
            return 1
        if not flags_allow_placeholder("-DFIRMWARE_VERSION=402 -DALLOW_PLACEHOLDER_CERTS"):
            print("ERROR: build flags did not carry ALLOW_PLACEHOLDER_CERTS", file=sys.stderr)
            return 1
        if flags_allow_placeholder("-DFIRMWARE_VERSION=402"):
            print("ERROR: the check bypassed without ALLOW_PLACEHOLDER_CERTS", file=sys.stderr)
            return 1
        inherited = effective_flag_text({
            "endnode_cell": "-DFIRMWARE_VERSION=402\n\t-DALLOW_PLACEHOLDER_CERTS",
            "endnode_wifi": "${env:endnode_cell.build_flags}\n\t-DFEATURE_WIFI",
        }, "endnode_wifi")
        if not flags_allow_placeholder(inherited):
            print("ERROR: wifi env did not inherit ALLOW_PLACEHOLDER_CERTS", file=sys.stderr)
            return 1
        plain = effective_flag_text({
            "endnode_cell": "-DFIRMWARE_VERSION=402",
            "endnode_wifi": "${env:endnode_cell.build_flags}\n\t-DFEATURE_WIFI",
        }, "endnode_wifi")
        if flags_allow_placeholder(plain):
            print("ERROR: wifi env bypassed without the flag", file=sys.stderr)
            return 1
    finally:
        shutil.rmtree(root)
    print("certificate header check self-test ok")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    allow = "--allow" in argv
    args = [arg for arg in argv[1:] if not arg.startswith("--")]
    certs = Path(args[0]) if args else Path("endnode") / "certs"
    return run_check(certs, allow)


_under_pio = False
try:
    Import("env")  # noqa: F821  PlatformIO injects Import.
    _under_pio = True
except NameError:
    env = None

if _under_pio:
    _project = Path(env["PROJECT_DIR"])  # noqa: F821
    _code = run_check(_project / "endnode" / "certs", allow_from_env(env))  # noqa: F821
    if _code != 0:
        env.Exit(_code)  # noqa: F821
elif __name__ == "__main__":
    raise SystemExit(main(sys.argv))
