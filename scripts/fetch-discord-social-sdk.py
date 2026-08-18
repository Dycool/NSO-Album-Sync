#!/usr/bin/env python3
"""Prepare the official Discord Social SDK archive for local/CI builds."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import tempfile
import urllib.request
import zipfile
from typing import Optional

DEFAULT_VERSION = "1.10.18369"
USER_AGENT = "nso-album-sync/2.0.0 (+https://github.com/Dycool/NSO-Album-Sync)"


def safe_extract(archive: Path, destination: Path) -> None:
    destination = destination.resolve()
    with zipfile.ZipFile(archive) as zf:
        bad_member = zf.testzip()
        if bad_member is not None:
            raise RuntimeError(f"Corrupt Discord SDK archive member: {bad_member}")
        for info in zf.infolist():
            target = (destination / info.filename).resolve()
            if target != destination and destination not in target.parents:
                raise RuntimeError(f"Unsafe path in Discord SDK archive: {info.filename}")
        zf.extractall(destination)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_sha256(path: Path, expected: str) -> None:
    expected = expected.strip().lower()
    if not expected:
        return
    actual = sha256_file(path)
    if actual != expected:
        raise RuntimeError(
            f"Discord Social SDK SHA-256 mismatch ({actual} != {expected})"
        )


def download(url: str, destination: Path) -> None:
    if not url.lower().startswith("https://"):
        raise RuntimeError("DISCORD_SOCIAL_SDK_URL must use HTTPS")
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response:
        final_url = response.geturl()
        if not final_url.lower().startswith("https://"):
            raise RuntimeError("Discord Social SDK download redirected away from HTTPS")
        with destination.open("wb") as output:
            shutil.copyfileobj(response, output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default=DEFAULT_VERSION)
    parser.add_argument("--destination", default="third_party")
    args = parser.parse_args()

    destination = Path(args.destination)
    sdk_root = destination / "discord_social_sdk"
    version_marker = sdk_root / ".nso-sdk-version"
    required = [
        sdk_root / "include" / "discordpp.h",
        sdk_root / "include" / "cdiscord.h",
    ]

    # A manually extracted SDK is accepted as-is. SDKs prepared by this helper
    # get a marker so version bumps cannot silently reuse an older runtime.
    if all(path.is_file() for path in required):
        if not version_marker.exists() or version_marker.read_text().strip() == args.version:
            return 0

    destination.mkdir(parents=True, exist_ok=True)
    archive_name = f"DiscordSocialSdk-{args.version}.zip"
    local_archive = destination / archive_name

    explicit_archive = os.environ.get("DISCORD_SOCIAL_SDK_ARCHIVE", "").strip()
    direct_url = os.environ.get("DISCORD_SOCIAL_SDK_URL", "").strip()
    expected_sha256 = os.environ.get("DISCORD_SOCIAL_SDK_SHA256", "").strip()

    source_archive: Optional[Path] = None
    temporary_directory: Optional[tempfile.TemporaryDirectory] = None
    try:
        if explicit_archive:
            source_archive = Path(explicit_archive)
            if not source_archive.is_file():
                raise RuntimeError(
                    f"DISCORD_SOCIAL_SDK_ARCHIVE does not exist: {source_archive}"
                )
        elif local_archive.is_file():
            source_archive = local_archive
        elif direct_url:
            temporary_directory = tempfile.TemporaryDirectory(
                prefix="nso-discord-sdk-"
            )
            source_archive = Path(temporary_directory.name) / archive_name
            download(direct_url, source_archive)
        else:
            raise RuntimeError(
                "Discord gates Social SDK downloads behind the Developer Portal. "
                f"Provide the official {archive_name} by placing it in third_party/, "
                "setting DISCORD_SOCIAL_SDK_ARCHIVE to a local file, or setting "
                "DISCORD_SOCIAL_SDK_URL to the archive's direct official HTTPS "
                "download URL. No Discord token/client secret is required."
            )

        verify_sha256(source_archive, expected_sha256)

        if sdk_root.exists():
            shutil.rmtree(sdk_root)
        safe_extract(source_archive, destination)
    finally:
        if temporary_directory is not None:
            temporary_directory.cleanup()

    if not all(path.is_file() for path in required):
        raise RuntimeError(
            "Discord Social SDK archive did not contain "
            "discord_social_sdk/include/discordpp.h and cdiscord.h"
        )
    version_marker.write_text(args.version + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
