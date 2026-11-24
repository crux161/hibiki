#!/usr/bin/env python3
"""Hibiki firmware bundle tool: pack / verify / install / show.

`pack` encodes both blobs into one base64 JSON bundle. `verify` and
`install` accept the bundle, a raw firmware tree, a lone base64 snippet of
either blob, or a directory of *.b64 snippets. Whatever the input, every
blob is checked against the pinned size and SHA-256 (the table in README.md)
before anything is installed.

Examples
--------
    scripts/fw_bundle.py pack                        # fw/ -> fw_package/
    scripts/fw_bundle.py verify  --src fw_package    # decode bundle + check
    scripts/fw_bundle.py install --src fw_package    # verify + write fw/
    scripts/fw_bundle.py verify  --src fw            # check a raw tree
    scripts/fw_bundle.py show    --src fw_package mbin_mxl_eagle_fw.bin

Hints
-----
if you look for a pastebin titled 120ec92d45fe082da1157ec3c391ab58d0d465728558b0ed629665b08ed8520c (the sha256 of the actual fw.json you need). check that shasum, then `fw_bundle.py show --src fw.json` and then `fw_bundle.py install --src fw.json`.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import os
import re
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

FORMAT = "hibiki-fw-bundle"
FORMAT_VERSION = 1
BUNDLE_NAME = "hibiki-fw-b64.json"
REPO_ROOT = Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class Firmware:
    path: str          # path relative to the firmware root (fw/ or the bundle)
    size: int
    sha256: str
    role: str


FIRMWARE = (
    Firmware(
        path="mbin_mxl_eagle_fw.bin",
        size=43997,
        sha256="8f77d5cb0de9111ed63c113cb2fd54a63e07a30040ae0b562cb025ab9c4e600d",
        role="MaxLinear Eagle tuner firmware, loaded by the app at runtime",
    ),
    Firmware(
        path="PT930_MN88553/PT930_pre_symbol_8051_region.bin",
        size=4212,
        sha256="0c3348a7f30fba3c9d3a1463f2583ce64f6af7922da4e60531f05473ce711d2a",
        role="Cypress EZ-USB FX2 bridge bootstrap, pushed by scripts/wake_tuner.py",
    ),
)
FW_BY_PATH = {fw.path: fw for fw in FIRMWARE}
KNOWN_BY_SHA = {fw.sha256: fw for fw in FIRMWARE}
REQUIRED_PATHS = tuple(fw.path for fw in FIRMWARE)


@dataclass
class Source:
    """Firmware blobs resolved from whatever input the user pointed at.

    expected: relpaths this source claims to provide (all required paths for
              a raw tree or bundle; the identified blob for a lone snippet).
    blobs:    relpath -> decoded bytes, for entries that decoded at all.
    errors:   relpath -> human-readable reason the entry is unusable.
    """

    label: str
    expected: tuple[str, ...]
    blobs: dict[str, bytes] = field(default_factory=dict)
    errors: dict[str, str] = field(default_factory=dict)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def identify(data: bytes) -> Optional[Firmware]:
    """Return the firmware whose pinned digest matches data, if any."""
    return KNOWN_BY_SHA.get(sha256_bytes(data))


def decode_snippet(raw: bytes) -> bytes:
    """Decode a base64 snippet (whitespace and #-comment lines tolerated)."""
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError("not ASCII base64 text") from exc
    body = "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#")
    )
    compact = re.sub(r"\s+", "", body)
    if not compact:
        raise ValueError("empty snippet")
    try:
        return base64.b64decode(compact, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise ValueError(f"invalid base64: {exc}") from exc


def write_atomic(path: Path, data: bytes) -> None:
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=path.name + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


# --- source resolution ------------------------------------------------------

def source_from_raw(root: Path) -> Source:
    blobs: dict[str, bytes] = {}
    errors: dict[str, str] = {}
    for fw in FIRMWARE:
        p = root / fw.path
        try:
            blobs[fw.path] = p.read_bytes()
        except FileNotFoundError:
            errors[fw.path] = f"not found ({p})"
        except OSError as exc:
            errors[fw.path] = f"cannot read {p}: {exc}"
    return Source(label=str(root), expected=REQUIRED_PATHS, blobs=blobs, errors=errors)


def source_from_bundle(path: Path) -> Source:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SystemExit(f"cannot read bundle {path}: {exc}")
    if not isinstance(doc, dict) or doc.get("format") != FORMAT or doc.get("version") != FORMAT_VERSION:
        raise SystemExit(f"{path} is not a {FORMAT} v{FORMAT_VERSION} bundle")

    entries: dict[str, dict] = {}
    for ent in doc.get("files") or []:
        if isinstance(ent, dict) and isinstance(ent.get("path"), str):
            entries[ent["path"]] = ent

    blobs: dict[str, bytes] = {}
    errors: dict[str, str] = {}
    for fw in FIRMWARE:
        ent = entries.get(fw.path)
        if ent is None:
            errors[fw.path] = "missing from bundle"
            continue
        payload = ent.get("base64")
        if not isinstance(payload, str):
            errors[fw.path] = "bundle entry has no base64 payload"
            continue
        compact = re.sub(r"\s+", "", payload)
        try:
            blobs[fw.path] = base64.b64decode(compact, validate=True)
        except (binascii.Error, ValueError) as exc:
            errors[fw.path] = f"invalid base64 payload ({exc})"
    return Source(label=str(path), expected=REQUIRED_PATHS, blobs=blobs, errors=errors)


def source_from_blob_file(path: Path, data: bytes) -> Source:
    """A single file: raw blob bytes or one base64 snippet, identified by hash."""
    fw = identify(data)
    if fw is not None:
        return Source(label=str(path), expected=(fw.path,), blobs={fw.path: data})
    decode_error = None
    decoded: Optional[bytes] = None
    try:
        decoded = decode_snippet(data)
    except ValueError as exc:
        decode_error = str(exc)
    if decoded is not None:
        fw = identify(decoded)
        if fw is not None:
            return Source(label=str(path), expected=(fw.path,), blobs={fw.path: decoded})
        raise SystemExit(
            f"{path}: base64 decodes to {len(decoded)} bytes "
            f"(sha256 {sha256_bytes(decoded)}), which matches no required firmware "
            f"blob — the snippet is corrupt or for a different firmware version"
        )
    raise SystemExit(
        f"{path} matches no known firmware file: not the raw bytes of a required "
        f"blob, and not valid base64 ({decode_error}; text sha256 {sha256_bytes(data)})"
    )


def source_from_snippets(root: Path, files: list[Path]) -> Source:
    blobs: dict[str, bytes] = {}
    for p in files:
        try:
            decoded = decode_snippet(p.read_bytes())
        except OSError as exc:
            raise SystemExit(f"{p}: cannot read: {exc}")
        except ValueError as exc:
            raise SystemExit(f"{p}: cannot decode snippet: {exc}")
        fw = identify(decoded)
        if fw is None:
            raise SystemExit(
                f"{p}: decodes to {len(decoded)} bytes but matches no known firmware "
                f"(sha256 {sha256_bytes(decoded)})"
            )
        if fw.path in blobs:
            raise SystemExit(f"{p}: duplicate snippet for {fw.path}")
        blobs[fw.path] = decoded
    expected = tuple(fw.path for fw in FIRMWARE if fw.path in blobs)
    return Source(label=str(root), expected=expected, blobs=blobs)


def resolve_source(src: Optional[str]) -> Source:
    """Accept a bundle, a raw tree, a lone blob/snippet file, or a snippet dir."""
    if src is None:
        bundle = REPO_ROOT / "fw_package" / BUNDLE_NAME
        if bundle.is_file():
            return source_from_bundle(bundle)
        raw = REPO_ROOT / "fw"
        if raw.is_dir():
            return source_from_raw(raw)
        raise SystemExit("no --src given and neither fw_package/ nor fw/ exists")

    path = Path(src)
    if path.is_dir():
        bundle = path / BUNDLE_NAME
        if bundle.is_file():
            return source_from_bundle(bundle)
        snippets = sorted(path.rglob("*.b64"))
        if snippets:
            return source_from_snippets(path, snippets)
        return source_from_raw(path)
    if path.is_file():
        data = path.read_bytes()
        if path.suffix.lower() == ".json" or data.lstrip()[:1] == b"{":
            return source_from_bundle(path)
        return source_from_blob_file(path, data)
    raise SystemExit(f"source not found: {path}")


# --- commands ---------------------------------------------------------------

def cmd_pack(args: argparse.Namespace) -> int:
    src = Path(args.src) if args.src else REPO_ROOT / "fw"
    out = Path(args.out) if args.out else REPO_ROOT / "fw_package"
    if not src.is_dir():
        print(f"[-] raw firmware directory not found: {src}", file=sys.stderr)
        return 1

    entries = []
    problems = []
    for fw in FIRMWARE:
        p = src / fw.path
        try:
            data = p.read_bytes()
        except OSError as exc:
            problems.append(f"{fw.path}: cannot read {p}: {exc}")
            continue
        digest = sha256_bytes(data)
        if len(data) != fw.size or digest != fw.sha256:
            problems.append(
                f"{fw.path}: does not match the pinned firmware "
                f"(expected {fw.size} bytes, sha256 {fw.sha256}; "
                f"got {len(data)} bytes, sha256 {digest})"
            )
            continue
        entries.append(
            {
                "path": fw.path,
                "size": fw.size,
                "sha256": fw.sha256,
                "role": fw.role,
                "base64": base64.b64encode(data).decode("ascii"),
            }
        )

    if problems:
        print("[-] refusing to pack; source failed verification:", file=sys.stderr)
        for prob in problems:
            print(f"      {prob}", file=sys.stderr)
        return 1

    doc = {
        "format": FORMAT,
        "version": FORMAT_VERSION,
        "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "generator": "scripts/fw_bundle.py",
        "note": (
            "..."
        ),
        "files": entries,
    }
    out.mkdir(parents=True, exist_ok=True)
    target = out / BUNDLE_NAME
    write_atomic(target, (json.dumps(doc, indent=2) + "\n").encode("utf-8"))
    print(f"[+] packed {len(entries)} firmware file(s) -> {target} ({target.stat().st_size} bytes)")
    print("    Base64 is an encoding, not a license: do not commit or redistribute the bundle.")
    return 0


def _verify_source(source: Source, quiet_ok: bool = False) -> tuple[int, int]:
    """Print per-file results; return (ok, failed) counts."""
    print(f"Verifying {len(source.expected)} firmware file(s) from {source.label}")
    ok = failed = 0
    for rel in source.expected:
        fw = FW_BY_PATH[rel]
        data = source.blobs.get(rel)
        if data is None:
            print(f"  FAIL  {rel}")
            print(f"        {source.errors.get(rel, 'missing')}")
            failed += 1
            continue
        digest = sha256_bytes(data)
        problems = []
        if len(data) != fw.size:
            problems.append(f"size mismatch: expected {fw.size} bytes, got {len(data)}")
        if digest != fw.sha256:
            problems.append("sha256 mismatch")
        if problems:
            print(f"  FAIL  {rel}")
            for prob in problems:
                print(f"        {prob}")
            if digest != fw.sha256:
                print(f"        expected {fw.sha256}")
                print(f"        actual   {digest}")
            failed += 1
        elif quiet_ok:
            ok += 1
        else:
            print(f"  OK    {rel}")
            print(f"        {len(data)} bytes  sha256 {digest}")
            ok += 1
    return ok, failed


def cmd_verify(args: argparse.Namespace) -> int:
    source = resolve_source(args.src)
    ok, failed = _verify_source(source)
    total = len(source.expected)
    if failed:
        print(f"[-] {failed} of {total} firmware file(s) FAILED verification", file=sys.stderr)
        return 1
    print(f"[+] all {ok} firmware file(s) verified")
    return 0


def cmd_install(args: argparse.Namespace) -> int:
    source = resolve_source(args.src)
    dest = Path(args.dest) if args.dest else REPO_ROOT / "fw"

    # Check everything first; write nothing unless the whole source is good.
    failures = []
    for rel in source.expected:
        fw = FW_BY_PATH[rel]
        data = source.blobs.get(rel)
        if data is None:
            failures.append(f"{rel}: {source.errors.get(rel, 'missing')}")
            continue
        digest = sha256_bytes(data)
        if len(data) != fw.size:
            failures.append(f"{rel}: size mismatch (expected {fw.size}, got {len(data)})")
        elif digest != fw.sha256:
            failures.append(f"{rel}: sha256 mismatch (expected {fw.sha256}, got {digest})")
    if failures:
        print("[-] refusing to install; source failed verification:", file=sys.stderr)
        for failure in failures:
            print(f"      {failure}", file=sys.stderr)
        return 1

    written = 0
    for rel in source.expected:
        fw = FW_BY_PATH[rel]
        data = source.blobs[rel]
        target = dest / rel
        if target.exists():
            try:
                current = target.read_bytes()
            except OSError as exc:
                print(f"[-] cannot read existing {target}: {exc}", file=sys.stderr)
                return 1
            if sha256_bytes(current) == fw.sha256:
                print(f"[=] {target}  already installed and verified")
                continue
            if not args.force:
                print(
                    f"[-] {target} exists with different content; "
                    f"re-run with --force to replace it",
                    file=sys.stderr,
                )
                return 1
        target.parent.mkdir(parents=True, exist_ok=True)
        write_atomic(target, data)
        print(f"[+] installed {target}  ({len(data)} bytes, sha256 {fw.sha256[:16]}...)")
        written += 1

    print(f"[+] {len(source.expected)} firmware file(s) verified, {written} written to {dest}")
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    source = resolve_source(args.src)
    wanted = args.path
    if wanted is None:
        if len(source.expected) != 1:
            print("[-] source provides multiple files; name one of:", file=sys.stderr)
            for rel in source.expected:
                print(f"      {rel}", file=sys.stderr)
            return 2
        wanted = source.expected[0]
    if wanted not in source.blobs:
        print(f"[-] {wanted}: {source.errors.get(wanted, 'not in this source')}", file=sys.stderr)
        return 2
    data = source.blobs[wanted]
    b64 = base64.b64encode(data).decode("ascii")
    print(f"# {wanted}  ({len(data)} bytes, sha256 {FW_BY_PATH[wanted].sha256})")
    for i in range(0, len(b64), 76):
        print(b64[i:i + 76])
    return 0


# --- CLI --------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="fw_bundle.py",
        description="Pack, verify, and install Hibiki's proprietary firmware blobs.",
        epilog=(
            "SOURCE may be a raw firmware tree (--src fw), the bundle file or its "
            "directory (fw_package), a lone raw blob or base64 snippet, or a "
            "directory of *.b64 snippets. verify/install default to fw_package/ "
            "when present, else fw/."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack", help="encode a raw firmware tree into a base64 bundle")
    p.add_argument("--src", help="raw firmware root (default: fw/)")
    p.add_argument("--out", help="bundle output directory (default: fw_package/)")
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("verify", help="decode if needed and verify size + SHA-256")
    p.add_argument("--src", help="source to verify (default: fw_package/, else fw/)")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("install", help="verify a source, then install it into the firmware dir")
    p.add_argument("--src", help="source to install (default: fw_package/, else fw/)")
    p.add_argument("--dest", help="firmware root to write (default: fw/)")
    p.add_argument("--force", action="store_true", help="replace existing files that differ")
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("show", help="print a blob as a bare base64 snippet")
    p.add_argument("--src", help="source to read (default: fw_package/, else fw/)")
    p.add_argument("path", nargs="?", help="firmware path to print (required when the source has several)")
    p.set_defaults(func=cmd_show)

    return ap


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
