#!/usr/bin/env python3
"""Fail closed if the Rust safety boundary is weakened."""

from __future__ import annotations

import pathlib
import re
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    print(f"safety gate: {message}", file=sys.stderr)
    raise SystemExit(1)


manifest_path = ROOT / "Cargo.toml"
manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
package = manifest.get("package", {})
if package.get("build") is not False:
    fail("Cargo.toml must set package.build = false")
if package.get("autobins") is not False:
    fail("Cargo.toml must set package.autobins = false")

rust_lints = manifest.get("lints", {}).get("rust", {})
clippy_lints = manifest.get("lints", {}).get("clippy", {})
if rust_lints.get("unsafe_code") != "forbid":
    fail("[lints.rust] unsafe_code must be forbid")
if clippy_lints.get("transmute_ptr_to_ptr") != "forbid":
    fail("[lints.clippy] transmute_ptr_to_ptr must be forbid")
if clippy_lints.get("undocumented_unsafe_blocks") != "forbid":
    fail("[lints.clippy] undocumented_unsafe_blocks must be forbid")

if (ROOT / "build.rs").exists():
    fail("build.rs is prohibited")

for crate_root in (ROOT / "src/lib.rs", ROOT / "src/main.rs"):
    text = crate_root.read_text(encoding="utf-8")
    if "#![forbid(unsafe_code)]" not in text:
        fail(f"{crate_root.relative_to(ROOT)} must contain #![forbid(unsafe_code)]")

source_roots = [ROOT / "src", ROOT / "tests", ROOT / "examples", ROOT / "benches"]
crate_override = re.compile(r"#!\s*\[\s*(?:allow|warn)\s*\(")
unsafe_construct = re.compile(r"\bunsafe\s*(?:\{|fn\b|impl\b|trait\b|extern\b)")
ffi_construct = re.compile(r"\bextern\s*\"(?:C|system)\"")
raw_pointer = re.compile(r"\*\s*(?:const|mut)\s+[A-Za-z_]")

for source_root in source_roots:
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

print("safety gate: OK")
