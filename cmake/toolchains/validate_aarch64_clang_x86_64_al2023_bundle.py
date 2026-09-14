#!/usr/bin/env python3
"""Authenticate the complete consumed closure of the coop x86 cross bundle."""

import argparse
import hashlib
import json
import stat
import sys
from pathlib import Path, PurePosixPath
from typing import Optional


COMPONENT_PATHS = {
    "launcher-payload": "bin/clang",
    "clang-payload": "usr/lib64/llvm18/bin/clang-18",
    "llvm-runtime-manifest": "native-tools.sha256",
    "al2023-x86_64-sysroot-manifest": "sysroot.manifest.json",
    "al2023-x86_64-rpm-manifest": "rpms.sha256",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_regular(path: Path) -> bool:
    try:
        return stat.S_ISREG(path.lstat().st_mode)
    except OSError:
        return False


def is_directory(path: Path) -> bool:
    try:
        return stat.S_ISDIR(path.lstat().st_mode)
    except OSError:
        return False


def regular_files(root: Path, scopes: tuple[str, ...]) -> set[str]:
    result: set[str] = set()
    for scope in scopes:
        base = root / scope
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if is_regular(path):
                result.add(path.relative_to(root).as_posix())
    return result


def symlinks(root: Path, scopes: tuple[str, ...]) -> set[str]:
    result: set[str] = set()
    for scope in scopes:
        base = root / scope
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.is_symlink():
                result.add(path.relative_to(root).as_posix())
    return result


def safe_relative(raw: str, require_dot_prefix: bool) -> Optional[str]:
    if require_dot_prefix and not raw.startswith("./"):
        return None
    path = PurePosixPath(raw)
    if path.is_absolute() or ".." in path.parts:
        return None
    normalized = path.as_posix()
    if normalized == ".":
        return None
    return normalized.removeprefix("./")


def parse_checksum_manifest(path: Path) -> tuple[dict[str, str], list[str]]:
    entries: dict[str, str] = {}
    errors: list[str] = []
    if not is_regular(path):
        return {}, [f"{path.name} must be a regular file"]
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        return {}, [f"cannot read {path.name}: {exc}"]
    for number, line in enumerate(lines, 1):
        if len(line) < 67 or line[64] != " " or line[65] not in " *":
            errors.append(f"{path.name}:{number}: malformed sha256sum entry")
            continue
        digest, raw = line[:64], line[66:]
        relative = safe_relative(raw, require_dot_prefix=False)
        if len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest):
            errors.append(f"{path.name}:{number}: invalid SHA-256")
        elif relative is None:
            errors.append(f"{path.name}:{number}: unsafe path {raw!r}")
        elif relative in entries:
            errors.append(f"{path.name}:{number}: duplicate path {relative!r}")
        else:
            entries[relative] = digest
    return entries, errors


def validate_checksum_closure(
    root: Path,
    manifest_name: str,
    base_name: str,
    scopes: tuple[str, ...],
) -> list[str]:
    manifest, errors = parse_checksum_manifest(root / manifest_name)
    base = root / base_name
    if not is_directory(base):
        errors.append(f"{manifest_name}: closure root must be a non-symlink directory: {base_name}")
        return errors
    for scope in scopes:
        if not is_directory(base / scope):
            errors.append(
                f"{manifest_name}: scope root must be a non-symlink directory: {scope}"
            )
    if errors:
        return errors
    actual = regular_files(base, scopes)
    listed = set(manifest)
    for path in sorted(actual - listed):
        errors.append(f"{manifest_name}: unlisted regular file {path}")
    for path in sorted(listed - actual):
        errors.append(f"{manifest_name}: listed path is not a regular file {path}")
    for path, expected in manifest.items():
        actual_path = base / path
        if is_regular(actual_path) and sha256(actual_path) != expected:
            errors.append(f"{manifest_name}: checksum mismatch for {path}")
    return errors


def validate_components(root: Path, manifest: object) -> list[str]:
    if not isinstance(manifest, list):
        return ["manifest components must be a list"]
    errors: list[str] = []
    by_name: dict[str, object] = {}
    for index, component in enumerate(manifest):
        if not isinstance(component, dict):
            errors.append(f"manifest components[{index}] must be an object")
            continue
        name = component.get("name")
        if not isinstance(name, str):
            errors.append(f"manifest components[{index}].name must be a string")
        elif name in by_name:
            errors.append(f"manifest component name duplicated: {name}")
        else:
            by_name[name] = component
    for name in sorted(set(COMPONENT_PATHS) - set(by_name)):
        errors.append(f"manifest component missing: {name}")
    for name in sorted(set(by_name) - set(COMPONENT_PATHS)):
        errors.append(f"manifest component unexpected: {name}")
    for name, expected_path in COMPONENT_PATHS.items():
        component = by_name.get(name)
        if not isinstance(component, dict):
            continue
        if component.get("path") != expected_path:
            errors.append(f"manifest component {name} has unexpected path")
            continue
        digest = component.get("sha256")
        if not isinstance(digest, str) or len(digest) != 64 or any(
            char not in "0123456789abcdef" for char in digest
        ):
            errors.append(f"manifest component {name} has invalid SHA-256")
            continue
        path = root / expected_path
        if not is_regular(path):
            errors.append(f"manifest component {name} is not a regular file")
        elif sha256(path) != digest:
            errors.append(f"manifest component {name} checksum mismatch")
    return errors


def validate_sysroot(root: Path) -> list[str]:
    errors: list[str] = []
    manifest_path = root / "sysroot.manifest.json"
    if not is_regular(manifest_path):
        return ["sysroot.manifest.json must be a regular file"]
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"cannot read sysroot.manifest.json: {exc}"]
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
        return ["sysroot.manifest.json schema_version must be 1"]

    def entries(key: str, value_key: str) -> dict[str, str]:
        result: dict[str, str] = {}
        values = manifest.get(key)
        if not isinstance(values, list):
            errors.append(f"sysroot.manifest.json {key} must be a list")
            return result
        for index, item in enumerate(values):
            if not isinstance(item, dict):
                errors.append(f"sysroot {key}[{index}] must be an object")
                continue
            raw, value = item.get("path"), item.get(value_key)
            relative = safe_relative(raw, require_dot_prefix=True) if isinstance(raw, str) else None
            if relative is None or not isinstance(value, str) or relative in result:
                errors.append(f"sysroot {key}[{index}] is invalid")
            else:
                result[relative] = value
        return result

    files = entries("regular_files", "sha256")
    links = entries("symlinks", "target")
    sysroot = root / "sysroot"
    if not is_directory(sysroot):
        return errors + ["sysroot must be a non-symlink directory"]
    actual_files = regular_files(sysroot, (".",))
    actual_links = symlinks(sysroot, (".",))
    for path in sorted(actual_files ^ set(files)):
        errors.append(f"sysroot.manifest.json regular-file inventory mismatch: {path}")
    for path in sorted(actual_links ^ set(links)):
        errors.append(f"sysroot.manifest.json symlink inventory mismatch: {path}")
    for path, expected in files.items():
        candidate = sysroot / path
        if is_regular(candidate):
            if len(expected) != 64 or any(char not in "0123456789abcdef" for char in expected):
                errors.append(f"sysroot.manifest.json invalid checksum for {path}")
            elif sha256(candidate) != expected:
                errors.append(f"sysroot.manifest.json checksum mismatch for {path}")
    for path, expected in links.items():
        candidate = sysroot / path
        if candidate.is_symlink():
            if candidate.readlink().as_posix() != expected:
                errors.append(f"sysroot.manifest.json symlink target mismatch for {path}")
    return errors


def validate(root: Path) -> list[str]:
    errors: list[str] = []
    manifest_path = root / "manifest.json"
    if not is_regular(manifest_path):
        return ["manifest.json must be a regular file"]
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"cannot read manifest.json: {exc}"]
    if not isinstance(manifest, dict):
        return ["manifest.json is not an object"]
    errors.extend(validate_components(root, manifest.get("components")))
    errors.extend(validate_checksum_closure(root, "native-tools.sha256", ".", ("bin", "usr", "host")))
    for path in sorted(symlinks(root, ("bin", "usr", "host"))):
        errors.append(f"native closure symlink forbidden: {path}")
    errors.extend(validate_sysroot(root))
    errors.extend(validate_checksum_closure(root, "rpms.sha256", "rpms", (".",)))
    for path in sorted(symlinks(root / "rpms", (".",))):
        errors.append(f"RPM closure symlink forbidden: {path}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    errors = validate(args.root.resolve())
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
