#!/usr/bin/env python3
"""Fetch the official Discord Social SDK archive without credentials."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile
import urllib.error
import urllib.request
import zipfile
from typing import Optional, Tuple

DEFAULT_VERSION = "1.10.18369"
API_BASE = "https://discord.com/api/v10/social-sdk/releases"
USER_AGENT = "nso-album-sync/2.0.0 (+https://github.com/Dycool/NSO-Album-Sync)"


def safe_extract(archive: Path, destination: Path) -> None:
    destination = destination.resolve()
    with zipfile.ZipFile(archive) as zf:
        for info in zf.infolist():
            target = (destination / info.filename).resolve()
            if target != destination and destination not in target.parents:
                raise RuntimeError(f"Unsafe path in Discord SDK archive: {info.filename}")
        zf.extractall(destination)


def download(url: str, destination: Path, expected_size: Optional[int]) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=60) as response, destination.open("wb") as output:
        shutil.copyfileobj(response, output)
    if expected_size and destination.stat().st_size != expected_size:
        raise RuntimeError(
            "Discord Social SDK download size did not match release metadata "
            f"({destination.stat().st_size} != {expected_size})"
        )


def official_artifact(version: str) -> Tuple[str, Optional[int]]:
    request = urllib.request.Request(
        f"{API_BASE}/{version}", headers={"User-Agent": USER_AGENT}
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = json.load(response)
    except (urllib.error.URLError, urllib.error.HTTPError) as error:
        status = getattr(error, "code", "network error")
        raise RuntimeError(
            f"Discord Social SDK metadata request failed ({status}). "
            "Download the official standalone C++ SDK archive from the Discord "
            "Developer Portal and set DISCORD_SOCIAL_SDK_ARCHIVE to its path."
        ) from error

    expected = f"DiscordSocialSdk-{version}.zip"
    for artifact in payload.get("artifacts", []):
        if artifact.get("filename") != expected:
            continue
        url = artifact.get("download_url", "")
        size = artifact.get("size_bytes")
        if url.startswith("https://"):
            return url, size if isinstance(size, int) and size > 0 else None
    raise RuntimeError(f"Discord did not publish {expected} in the release metadata")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default=DEFAULT_VERSION)
    parser.add_argument("--destination", default="third_party")
    args = parser.parse_args()

    destination = Path(args.destination)
    sdk_root = destination / "discord_social_sdk"
    version_marker = sdk_root / ".nso-sdk-version"
    required = [sdk_root / "include" / "discordpp.h", sdk_root / "include" / "cdiscord.h"]

    # A manually extracted SDK is accepted as-is. For SDKs fetched by this
    # helper, the marker prevents a version bump from silently reusing an older
    # native runtime.
    if all(path.is_file() for path in required):
        if not version_marker.exists() or version_marker.read_text().strip() == args.version:
            return 0

    destination.mkdir(parents=True, exist_ok=True)
    archive_name = f"DiscordSocialSdk-{args.version}.zip"
    archive = destination / archive_name

    explicit_archive = os.environ.get("DISCORD_SOCIAL_SDK_ARCHIVE")
    source_archive: Optional[Path] = None
    temporary_directory: Optional[tempfile.TemporaryDirectory] = None
    try:
        if explicit_archive:
            source_archive = Path(explicit_archive)
            if not source_archive.is_file():
                raise RuntimeError(
                    f"DISCORD_SOCIAL_SDK_ARCHIVE does not exist: {source_archive}"
                )
        elif archive.is_file():
            source_archive = archive
        else:
            url, size = official_artifact(args.version)
            temporary_directory = tempfile.TemporaryDirectory(prefix="nso-discord-sdk-")
            source_archive = Path(temporary_directory.name) / archive_name
            download(url, source_archive, size)

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
