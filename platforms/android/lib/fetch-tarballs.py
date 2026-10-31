#!/usr/bin/env python3
"""Downloads the four libmpv-android tarballs the mpv-android AAR packs.

Usage: fetch-tarballs.py [--key KEY]

Without --key: the key, sha256 checksums and locations artifacts.json records
at this checkout (each tarball's own assetBase, else the section's). With
--key: any published key, from whichever release of this repository holds
its tarballs (binaries-android or a publish run's own), checked against the
sha256 digest GitHub records for each asset (set GH_TOKEN to lift the API's
anonymous rate limit). The files land in
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


def find_release(names: list[str]) -> tuple[str, dict[str, str]]:
    """The download base of the release holding every name, and their sha256 digests.

    The releases list embeds each release's assets, newest release first.
    """
    api = keys.REPOSITORY_URL.replace("https://github.com/", "https://api.github.com/repos/")
    page = 1
    while True:
        url = f"{api}/releases?per_page=100&page={page}"
        with urllib.request.urlopen(request(url)) as response:
            releases = json.load(response)
        if not releases:
            raise SystemExit(f"no release of {keys.REPOSITORY_URL} holds {', '.join(names)}")
        for release in releases:
            digests = {
                asset["name"]: (asset.get("digest") or "").removeprefix("sha256:")
                for asset in release.get("assets", [])
            }
            if all(digests.get(name) for name in names):
                return f"{keys.DOWNLOAD_BASE}/{release['tag_name']}", digests
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
        asset_base, digests = find_release(list(names.values()))
        wanted = {variant: (name, digests[name], asset_base) for variant, name in names.items()}
    else:
        section = keys.load_manifest(root)["platforms"].get("android") or {}
        entry = (section.get("libraries") or {}).get(ARTIFACT)
        if not entry:
            raise SystemExit(f"artifacts.json records no {ARTIFACT}")
        key = entry["key"]
        wanted = {}
        for variant in spec.variants:
            recorded = entry["prebuilt"][variant]
            wanted[variant] = (
                recorded["asset"], recorded["checksum"], keys.asset_location(section, recorded)
            )

    dest = here / "build" / "tarballs" / key
    dest.mkdir(parents=True, exist_ok=True)
    for variant, (name, checksum, asset_base) in wanted.items():
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
