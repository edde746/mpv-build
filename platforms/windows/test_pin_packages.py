#!/usr/bin/env python3
"""Unit tests for pin_packages.py against fixture copies of the real
mpv-winbuild-cmake package files (testdata/, see testdata/PROVENANCE).

Run: python3 platforms/windows/test_pin_packages.py
"""

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESTDATA = HERE / "testdata"
sys.path.insert(0, str(HERE))

import pin_packages  # noqa: E402

PINS = {
    "mpv": {
        "version": "v0.41.0",
        "url": "https://github.com/mpv-player/mpv",
        "ref": "v0.41.0",
        "commit": "41f6a645068483470267271e1d09966ca3b9f413",
    },
    "ffmpeg": {
        "version": "n8.0.1",
        "url": "https://github.com/FFmpeg/FFmpeg",
        "ref": "n8.0.1",
        "commit": "894da5ca7d742e4429ffb2af534fcda0103ef593",
    },
    "libass": {
        "version": "0.18.3",
        "url": "https://github.com/edde746/libass",
        "ref": "0.18.3",
        "commit": "76cdb2bc174828aac74a458d38a0786cb7af922d",
    },
}

PATCH = """diff --git a/a.c b/a.c
index 0000000..1111111 100644
--- a/a.c
+++ b/a.c
@@ -1 +1 @@
-old
+new
"""


def keyword_sequence(text, keywords):
    """The keyword of each line whose first word is in `keywords`, in order."""
    out = []
    for line in text.splitlines():
        word = line.strip().split(" ", 1)[0] if line.strip() else ""
        if word in keywords:
            out.append(word)
    return out


class PinPackagesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="pin-packages-test-"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.packages = self.tmp / "winbuild" / "packages"
        self.packages.mkdir(parents=True)
        for name in ("mpv", "ffmpeg", "libass"):
            shutil.copyfile(TESTDATA / f"{name}.cmake", self.packages / f"{name}.cmake")

        # Synthetic repo root: mpv has a two-entry windows series, ffmpeg and
        # libass have empty series (matching the real repo today).
        self.repo = self.tmp / "repo"
        pool = self.repo / "patches" / "mpv" / "pool"
        pool.mkdir(parents=True)
        (pool / "0001-first.patch").write_text(PATCH)
        (pool / "0002-second.patch").write_text(PATCH)
        (self.repo / "patches" / "mpv" / "series.common").write_text("0001-first.patch\n")
        (self.repo / "patches" / "mpv" / "series.windows").write_text(
            "# windows-only entry\n0002-second.patch\n"
        )
        (self.repo / "patches" / "ffmpeg").mkdir(parents=True)
        (self.repo / "patches" / "ffmpeg" / "series.windows").write_text("# empty\n")

    def run_pin(self, component):
        staged = pin_packages.stage_patches(self.repo, component, self.packages)
        path = self.packages / f"{component}.cmake"
        text = path.read_text()
        pinned = pin_packages.rewrite(text, component, PINS[component], bool(staged))
        path.write_text(pinned)
        return pinned, staged

    def run_all(self):
        return {c: self.run_pin(c) for c in ("mpv", "ffmpeg", "libass")}

    def test_fixtures_are_pristine(self):
        # The strip-before-inject idempotency contract is only safe because the
        # upstream files carry none of the keywords this script owns.
        for name in ("mpv", "ffmpeg", "libass"):
            text = (TESTDATA / f"{name}.cmake").read_text()
            self.assertEqual(
                keyword_sequence(text, set(pin_packages.INJECTED_KEYWORDS)), [],
                f"{name}.cmake fixture unexpectedly carries injected keywords",
            )

    def test_injected_block_matches_mbedtls_idiom(self):
        pinned, _ = self.run_pin("mpv")
        # The keyword shape upstream itself uses for a pinned+patched package,
        # taken from the mbedtls fixture rather than hardcoded here.
        idiom = ("PATCH_COMMAND", "UPDATE_COMMAND", "GIT_REMOTE_NAME", "GIT_TAG", "GIT_RESET")
        mbedtls = (TESTDATA / "mbedtls.cmake").read_text()
        self.assertEqual(
            keyword_sequence(pinned, set(idiom)),
            keyword_sequence(mbedtls, set(idiom)),
            "injected block does not follow the mbedtls.cmake keyword order",
        )
        # And the exact injected lines, contiguous, mbedtls-style 4-space indent.
        expected = (
            "    PATCH_COMMAND ${EXEC} git apply ${CMAKE_CURRENT_SOURCE_DIR}/mpv-*.patch\n"
            '    UPDATE_COMMAND ""\n'
            "    GIT_REMOTE_NAME origin\n"
            "    GIT_TAG v0.41.0\n"
            "    GIT_RESET 41f6a645068483470267271e1d09966ca3b9f413 # v0.41.0\n"
        )
        self.assertIn(expected, pinned)

    def test_patch_command_only_for_nonempty_series(self):
        results = self.run_all()
        self.assertIn("PATCH_COMMAND", results["mpv"][0])
        self.assertNotIn("PATCH_COMMAND", results["ffmpeg"][0])
        self.assertNotIn("PATCH_COMMAND", results["libass"][0])
        # ffmpeg/libass still get the pin block.
        for component in ("ffmpeg", "libass"):
            pins = PINS[component]
            self.assertIn(f"    GIT_RESET {pins['commit']} # {pins['version']}\n", results[component][0])
            self.assertIn("    GIT_REMOTE_NAME origin\n", results[component][0])

    def test_staged_patches_glob_in_series_order(self):
        _, staged = self.run_pin("mpv")
        self.assertEqual(staged, ["mpv-0001-0001-first.patch", "mpv-0002-0002-second.patch"])
        self.assertEqual(sorted(staged), staged, "glob order must equal series order")
        for name in staged:
            self.assertTrue((self.packages / name).is_file())

    def test_libass_points_at_edde746_fork(self):
        pinned, _ = self.run_pin("libass")
        self.assertIn("    GIT_REPOSITORY https://github.com/edde746/libass.git\n", pinned)
        self.assertNotIn("github.com/libass/libass", pinned)

    def test_ffmpeg_sparse_checkout_preserved(self):
        pinned, _ = self.run_pin("ffmpeg")
        self.assertIn('GIT_CLONE_POST_COMMAND "sparse-checkout set --no-cone /* !tests/ref/fate"', pinned)
        self.assertIn('GIT_CLONE_FLAGS "--sparse --filter=tree:0"', pinned)

    def test_idempotent_rerun(self):
        first = {c: (self.packages / f"{c}.cmake").read_text() for c in self.run_all()}
        second = {c: (self.packages / f"{c}.cmake").read_text() for c in self.run_all()}
        self.assertEqual(first, second)
        # Staged patch set converges too (stale files removed, same names).
        staged = sorted(p.name for p in self.packages.glob("*-*.patch"))
        self.assertEqual(staged, ["mpv-0001-0001-first.patch", "mpv-0002-0002-second.patch"])

    def test_overrides_windows_folds_into_pins(self):
        versions = {
            "components": {
                "ffmpeg": {
                    "version": "n8.0.1",
                    "url": "https://github.com/FFmpeg/FFmpeg",
                    "ref": "n8.0.1",
                    "commit": "894da5ca7d742e4429ffb2af534fcda0103ef593",
                    "overrides": {"windows": {"commit": "f" * 40, "ref": "windows-branch"}},
                }
            }
        }
        pins = pin_packages.resolved_pins(versions, "ffmpeg")
        self.assertEqual(pins["commit"], "f" * 40)
        self.assertEqual(pins["ref"], "windows-branch")
        self.assertEqual(pins["url"], "https://github.com/FFmpeg/FFmpeg")

    def test_missing_commit_fails(self):
        versions = {"components": {"mpv": {"version": "v1", "url": "u", "ref": "v1"}}}
        with self.assertRaises(SystemExit):
            pin_packages.resolved_pins(versions, "mpv")


if __name__ == "__main__":
    unittest.main(verbosity=2)
