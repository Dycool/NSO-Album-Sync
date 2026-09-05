#!/usr/bin/env python3
"""Fail closed if the Rust safety boundary is weakened."""

from __future__ import annotations

import pathlib
import re
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[1]
BOUNDARY = ROOT / "discord-social-sdk"
BOUNDARY_LIB = BOUNDARY / "src" / "lib.rs"
BOUNDARY_BUILD = BOUNDARY / "build.rs"


def fail(message: str) -> None:
    print(f"safety gate: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_manifest(path: pathlib.Path) -> dict:
    if not path.is_file():
        fail(f"missing manifest: {path.relative_to(ROOT)}")
    return tomllib.loads(path.read_text(encoding="utf-8"))


manifest = load_manifest(ROOT / "Cargo.toml")
package = manifest.get("package", {})
if package.get("build") is not False:
    fail("Cargo.toml must set package.build = false")
if package.get("autobins") is not False:
    fail("Cargo.toml must set package.autobins = false")

rust_lints = manifest.get("lints", {}).get("rust", {})
clippy_lints = manifest.get("lints", {}).get("clippy", {})
if rust_lints.get("unsafe_code") != "forbid":
    fail("root [lints.rust] unsafe_code must be forbid")
if clippy_lints.get("transmute_ptr_to_ptr") != "forbid":
    fail("root [lints.clippy] transmute_ptr_to_ptr must be forbid")
if clippy_lints.get("undocumented_unsafe_blocks") != "forbid":
    fail("root [lints.clippy] undocumented_unsafe_blocks must be forbid")

boundary_dependency = manifest.get("dependencies", {}).get("nso-discord-social-sdk", {})
if boundary_dependency.get("path") != "discord-social-sdk" or boundary_dependency.get("optional") is not True:
    fail("root must depend on optional path-only nso-discord-social-sdk boundary")
if "dep:nso-discord-social-sdk" not in manifest.get("features", {}).get("desktop", []):
    fail("desktop feature must enable the Discord Social SDK boundary")
if "discord-rich-presence" in manifest.get("dependencies", {}):
    fail("legacy Discord IPC dependency is prohibited")

boundary_manifest = load_manifest(BOUNDARY / "Cargo.toml")
boundary_rust_lints = boundary_manifest.get("lints", {}).get("rust", {})
boundary_clippy_lints = boundary_manifest.get("lints", {}).get("clippy", {})
if boundary_manifest.get("package", {}).get("name") != "nso-discord-social-sdk":
    fail("Discord unsafe exception must remain in the named boundary crate")
if boundary_rust_lints.get("unsafe_code") != "allow":
    fail("Discord boundary must explicitly scope unsafe_code = allow")
if boundary_rust_lints.get("unsafe_op_in_unsafe_fn") != "deny":
    fail("Discord boundary must deny unsafe operations in unsafe functions")
if boundary_clippy_lints.get("transmute_ptr_to_ptr") != "forbid":
    fail("Discord boundary must forbid transmute_ptr_to_ptr")
if boundary_clippy_lints.get("undocumented_unsafe_blocks") != "deny":
    fail("Discord boundary must deny undocumented unsafe blocks")

for crate_root in (ROOT / "src/lib.rs", ROOT / "src/main.rs"):
    text = crate_root.read_text(encoding="utf-8")
    if "#![forbid(unsafe_code)]" not in text:
        fail(f"{crate_root.relative_to(ROOT)} must contain #![forbid(unsafe_code)]")

if not BOUNDARY_BUILD.is_file():
    fail("Discord boundary build.rs is required")
if not (BOUNDARY / "src" / "shim.cpp").is_file():
    fail("Discord boundary C++ shim is required")
if not BOUNDARY_LIB.is_file():
    fail("Discord boundary Rust wrapper is required")

build_scripts = [path for path in ROOT.rglob("build.rs") if "third_party" not in path.parts and "target" not in path.parts]
if build_scripts != [BOUNDARY_BUILD]:
    rendered = ", ".join(str(path.relative_to(ROOT)) for path in build_scripts)
    fail(f"build.rs is allowed only at discord-social-sdk/build.rs; found: {rendered}")

crate_override = re.compile(r"#!\s*\[\s*(?:allow|warn)\s*\(")
unsafe_construct = re.compile(r"\bunsafe\s*(?:\{|fn\b|impl\b|trait\b|extern\b)")
ffi_construct = re.compile(r"\bextern\s*\"(?:C|system)\"")
raw_pointer = re.compile(r"\*\s*(?:const|mut)\s+[A-Za-z_]")

for source_root in (ROOT / "src", ROOT / "tests", ROOT / "examples", ROOT / "benches"):
    if not source_root.exists():
        continue
    for path in source_root.rglob("*.rs"):
        text = path.read_text(encoding="utf-8")
        relative = path.relative_to(ROOT)
        if crate_override.search(text):
            fail(f"crate-level allow/warn override is prohibited in {relative}")
        if unsafe_construct.search(text):
            fail(f"unsafe Rust construct is prohibited in {relative}")
        if ffi_construct.search(text):
            fail(f"FFI extern block is prohibited in {relative}")
        if raw_pointer.search(text):
            fail(f"raw pointer type is prohibited in {relative}")

boundary_text = BOUNDARY_LIB.read_text(encoding="utf-8")
if "#![allow(unsafe_code)]" not in boundary_text:
    fail("Discord boundary must explicitly declare #![allow(unsafe_code)]")
if "#![deny(unsafe_op_in_unsafe_fn)]" not in boundary_text:
    fail("Discord boundary must declare #![deny(unsafe_op_in_unsafe_fn)]")
without_expected_override = boundary_text.replace("#![allow(unsafe_code)]", "")
if crate_override.search(without_expected_override):
    fail("Discord boundary may not add any other crate-level allow/warn override")
if re.search(r"\bunsafe\s+(?:fn|impl|trait)\b", boundary_text):
    fail("Discord boundary may use unsafe blocks/extern declarations, not unsafe fn/impl/trait")

lines = boundary_text.splitlines()
for index, line in enumerate(lines):
    if "unsafe {" not in line:
        continue
    context = "\n".join(lines[max(0, index - 3):index])
    if "SAFETY:" not in context:
        fail(f"Discord boundary unsafe block on line {index + 1} lacks nearby SAFETY documentation")

build_text = BOUNDARY_BUILD.read_text(encoding="utf-8")
if unsafe_construct.search(build_text) or ffi_construct.search(build_text) or raw_pointer.search(build_text):
    fail("Discord boundary build.rs must itself remain safe Rust")

print("safety gate: OK (unsafe exception: discord-social-sdk/src/lib.rs only)")
