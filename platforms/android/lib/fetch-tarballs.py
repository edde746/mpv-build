#!/usr/bin/env python3
"""Downloads the four libmpv-android tarballs the mpv-android AAR packs.

Usage: fetch-tarballs.py [--key KEY]

Without --key: the key and sha256 checksums artifacts.json records at this
checkout. With --key: any published key, checked against the sha256 digest
GitHub records for each asset of the binaries-android release (set GH_TOKEN
to lift the API's anonymous rate limit). The files land in
platforms/android/lib/build/tarballs/<key>/ under their published names (kept
there: a file already present with the right sha256 is not fetched again),
and that directory is the one line printed on stdout.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import urllib.request
from pathlib import Path

here = Path(__file__).resolve().parent
root = here.parent.parent.parent
sys.path.insert(0, str(root / "scripts"))
import keys  # noqa: E402  (scripts/keys.py: the group, its variants and asset names)

ARTIFACT = "libmpv-android"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def request(url: str) -> urllib.request.Request:
    headers = {"User-Agent": "mpv-build fetch-tarballs"}
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        headers["Authorization"] = f"Bearer {token}"
    return urllib.request.Request(url, headers=headers)


def published_digests(asset_base: str) -> dict[str, str]:
    """Asset name -> sha256 for the release asset_base points at."""
    # https://github.com/<owner>/<repo>/releases/download/<tag>
    parts = asset_base.removeprefix("https://github.com/").split("/")
    owner, repo, tag = parts[0], parts[1], parts[4]
    api = f"https://api.github.com/repos/{owner}/{repo}"
    with urllib.request.urlopen(request(f"{api}/releases/tags/{tag}")) as response:
        release_id = json.load(response)["id"]
    digests: dict[str, str] = {}
    page = 1
    while True:
        url = f"{api}/releases/{release_id}/assets?per_page=100&page={page}"
        with urllib.request.urlopen(request(url)) as response:
            assets = json.load(response)
        if not assets:
            return digests
        for asset in assets:
            digest = asset.get("digest") or ""
            if digest.startswith("sha256:"):
                digests[asset["name"]] = digest.removeprefix("sha256:")
        page += 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--key", help="a published libmpv-android content key")
    args = parser.parse_args()

    group = keys.GROUPS["android"]
    spec = group.artifacts[ARTIFACT]
    if args.key:
        key = args.key
        names = {variant: spec.asset_pattern.format(key=key, variant=variant) for variant in spec.variants}
        digests = published_digests(group.asset_base)
        missing = [name for name in names.values() if name not in digests]
        if missing:
            raise SystemExit(f"not published on {group.asset_base}: {', '.join(missing)}")
        wanted = {variant: (name, digests[name]) for variant, name in names.items()}
        asset_base = group.asset_base
    else:
        section = keys.load_manifest(root)["platforms"].get("android") or {}
        entry = (section.get("libraries") or {}).get(ARTIFACT)
        if not entry:
            raise SystemExit(f"artifacts.json records no {ARTIFACT}")
        key = entry["key"]
        wanted = {
            variant: (entry["prebuilt"][variant]["asset"], entry["prebuilt"][variant]["checksum"])
            for variant in spec.variants
        }
        asset_base = section["assetBase"]

    dest = here / "build" / "tarballs" / key
    dest.mkdir(parents=True, exist_ok=True)
    for variant, (name, checksum) in wanted.items():
        target = dest / name
        if target.is_file() and sha256(target) == checksum:
            continue
        partial = dest / f"{name}.part"
        print(f"fetching {name}", file=sys.stderr)
        with urllib.request.urlopen(request(f"{asset_base}/{name}")) as response, partial.open("wb") as out:
            while chunk := response.read(1024 * 1024):
                out.write(chunk)
        actual = sha256(partial)
        if actual != checksum:
            partial.unlink()
            raise SystemExit(f"{name}: sha256 {actual} != {checksum}")
        partial.rename(target)
    print(dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
