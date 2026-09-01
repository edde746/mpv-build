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
    "mingw-w64": {
        "version": "master-2026-08-29",
        "url": "https://github.com/mingw-w64/mingw-w64",
        "ref": "master",
        "commit": "ca4cc40bdcda1aa3e9df68d5443c7ceaf1f212f9",
    },
    "llvm": {
        "version": "release-22.x-2026-06-15",
        "url": "https://github.com/llvm/llvm-project",
        "ref": "release/22.x",
        "commit": "ca7933e47d3a3451d81e72ac174dcb5aa28b59d1",
    },
    "svt-av1": {
        "version": "v3.1.2",
        "url": "https://gitlab.com/AOMediaCodec/SVT-AV1",
        "ref": "v3.1.2",
        "commit": "b33dcc56cc64fcb3b3569094af8ab1d0d81ab4c1",
    },
    "nv-codec-headers": {
        "version": "n13.0.19.1",
        "url": "https://github.com/FFmpeg/nv-codec-headers",
        "ref": "n13.0.19.1",
        "commit": "88fee5c37318c991a8762d423530f91681e32e3a",
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
        # GIT_TAG is the resolved commit, not the tag name: the tag value is
        # what lands in <pkg>-gitinfo.txt, the only graph input that dirties
        # the download step on a warm tree. A textually stable ref whose
        # target moved must invalidate.
        expected = (
            "    PATCH_COMMAND ${EXEC} "
            '"git reset --hard 41f6a645068483470267271e1d09966ca3b9f413 -q '
            '&& git apply ${CMAKE_CURRENT_SOURCE_DIR}/mpv-*.patch"\n'
            '    UPDATE_COMMAND ""\n'
            "    GIT_REMOTE_NAME origin\n"
            "    GIT_TAG 41f6a645068483470267271e1d09966ca3b9f413\n"
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

    def test_patch_command_resets_to_the_pinned_commit(self):
        # A patch step re-run alone (series-only change, or a warm-cache step
        # cascade) must converge instead of double-applying onto a patched
        # tree, so the injected command resets to the pin before applying.
        pinned, _ = self.run_pin("mpv")
        self.assertIn(f'"git reset --hard {PINS["mpv"]["commit"]} -q && git apply ', pinned)

    def test_mingw_fixture_is_pristine_and_pin_matches_idiom(self):
        # The toolchain source package pins through the same machinery; its
        # pristine upstream file has no GIT_TAG at all (implicit master tip).
        text = (TESTDATA / "mingw-w64.cmake").read_text()
        self.assertEqual(
            keyword_sequence(text, set(pin_packages.INJECTED_KEYWORDS)), [],
            "mingw-w64.cmake fixture unexpectedly carries injected keywords",
        )
        pins = PINS["mingw-w64"]
        pinned = pin_packages.rewrite(text, "mingw-w64", pins, False)
        self.assertIn(
            '    UPDATE_COMMAND ""\n'
            "    GIT_REMOTE_NAME origin\n"
            f"    GIT_TAG {pins['commit']}\n"
            f"    GIT_RESET {pins['commit']} # {pins['version']}\n",
            pinned,
        )
        self.assertNotIn("PATCH_COMMAND", pinned)
        # Idempotent like the payload rewrites.
        self.assertEqual(pin_packages.rewrite(pinned, "mingw-w64", pins, False), pinned)

    def test_llvm_branch_tag_is_replaced_by_the_pin(self):
        # llvm's pristine file carries upstream's own GIT_REMOTE_NAME and a
        # moving-branch GIT_TAG (release/22.x); the strip-before-inject
        # rewrite must replace them with the resolved commit, not stack a
        # second block next to them.
        text = (TESTDATA / "llvm.cmake").read_text()
        self.assertIn("    GIT_TAG release/22.x\n", text)
        pins = PINS["llvm"]
        pinned = pin_packages.rewrite(text, "llvm", pins, False)
        self.assertNotIn("GIT_TAG release/22.x", pinned)
        self.assertEqual(pinned.count("GIT_TAG "), 1)
        self.assertEqual(pinned.count("GIT_REMOTE_NAME "), 1)
        self.assertIn(
            '    UPDATE_COMMAND ""\n'
            "    GIT_REMOTE_NAME origin\n"
            f"    GIT_TAG {pins['commit']}\n"
            f"    GIT_RESET {pins['commit']} # {pins['ref']} {pins['version']}\n",
            pinned,
        )
        # The sparse-checkout clone flags must survive the rewrite.
        self.assertIn('GIT_CLONE_FLAGS "--sparse --filter=tree:0"', pinned)
        self.assertEqual(pin_packages.rewrite(pinned, "llvm", pins, False), pinned)

    def test_check_git_fixture_matches_audited_idiom(self):
        # neutralize_check_git string-matches the exact upstream injection
        # guard; a winbuild bump that reshapes it must fail loud, not
        # silently skip.
        text = (TESTDATA / "custom_steps.cmake").read_text()
        self.assertEqual(text.count(pin_packages.CHECK_GIT_ORIGINAL), 1)
        self.assertNotIn(pin_packages.CHECK_GIT_NEUTRALIZED, text)

    def test_svtav1_pin_matches_idiom(self):
        # svt-av1 pins to a 3.x release: SVT-AV1 4.0 removed a field the
        # pinned release ffmpeg still sets unguarded.
        text = (TESTDATA / "svtav1.cmake").read_text()
        pins = PINS["svt-av1"]
        pinned = pin_packages.rewrite(text, "svt-av1", pins, False)
        self.assertIn(
            '    UPDATE_COMMAND ""\n'
            "    GIT_REMOTE_NAME origin\n"
            f"    GIT_TAG {pins['commit']}\n"
            f"    GIT_RESET {pins['commit']} # {pins['version']}\n",
            pinned,
        )
        self.assertEqual(pin_packages.rewrite(pinned, "svt-av1", pins, False), pinned)

    def test_nvcodec_pin_matches_idiom(self):
        # nv-codec-headers pins to the 13.0 series: the 13.1 in-dev tip
        # reshapes NV_ENC_CLOCK_TIMESTAMP_SET, which n8.0.1's nvenc wrapper
        # still uses.
        text = (TESTDATA / "nvcodec-headers.cmake").read_text()
        pins = PINS["nv-codec-headers"]
        pinned = pin_packages.rewrite(text, "nv-codec-headers", pins, False)
        self.assertIn(
            '    UPDATE_COMMAND ""\n'
            "    GIT_REMOTE_NAME origin\n"
            f"    GIT_TAG {pins['commit']}\n"
            f"    GIT_RESET {pins['commit']} # {pins['version']}\n",
            pinned,
        )
        self.assertEqual(pin_packages.rewrite(pinned, "nv-codec-headers", pins, False), pinned)

    def test_ffmpeg_cuda_is_arch_gated(self):
        # The pinned release ffmpeg's ffnvcodec probe fails on
        # aarch64-w64-mingw32; the unconditional cuda enables become a
        # variable that is only set off-aarch64.
        text = (TESTDATA / "ffmpeg.cmake").read_text()
        self.assertIn(pin_packages.FFMPEG_CUDA_ORIGINAL, text)
        gated = pin_packages.gate_ffmpeg_cuda(text)
        self.assertNotIn(pin_packages.FFMPEG_CUDA_ORIGINAL, gated)
        self.assertIn("${ffmpeg_cuda}", gated)
        self.assertTrue(gated.startswith(pin_packages.FFMPEG_CUDA_GUARD))
        # The four flags survive, exactly once, inside the guard.
        for flag in ("--enable-cuda-llvm", "--enable-cuvid", "--enable-nvdec", "--enable-nvenc"):
            self.assertEqual(gated.count(flag), 1)
        self.assertEqual(pin_packages.gate_ffmpeg_cuda(gated), gated)

    def test_mpv_master_only_options_are_stripped(self):
        # meson hard-errors on unknown options; subrandr and libcurl landed
        # after v0.41.0.
        text = (TESTDATA / "mpv.cmake").read_text()
        self.assertIn("-Dsubrandr=enabled", text)
        self.assertIn("-Dlibcurl=enabled", text)
        gated = pin_packages.gate_mpv_options(text)
        self.assertNotIn("-Dsubrandr", gated)
        self.assertNotIn("-Dlibcurl", gated)
        # Only the two option lines vanish; the DEPENDS entry stays.
        self.assertIn("subrandr\n", gated)
        self.assertEqual(len(text.splitlines()) - 2, len(gated.splitlines()))
        self.assertEqual(pin_packages.gate_mpv_options(gated), gated)

    def test_ffmpeg_cuda_gate_survives_full_rewrite_cycle(self):
        # main() applies rewrite() then gate_ffmpeg_cuda() on every run; a
        # second full cycle must converge byte-identically.
        text = (TESTDATA / "ffmpeg.cmake").read_text()
        pins = PINS["ffmpeg"]
        once = pin_packages.gate_ffmpeg_cuda(pin_packages.rewrite(text, "ffmpeg", pins, False))
        twice = pin_packages.gate_ffmpeg_cuda(pin_packages.rewrite(once, "ffmpeg", pins, False))
        self.assertEqual(once, twice)

    def test_neutralize_check_git_suppresses_and_converges(self):
        text = (TESTDATA / "custom_steps.cmake").read_text()
        fixed = pin_packages.neutralize_check_git(text)
        self.assertNotIn(pin_packages.CHECK_GIT_ORIGINAL, fixed)
        self.assertIn(pin_packages.CHECK_GIT_NEUTRALIZED, fixed)
        # The step must be unreachable: its commands touch the download stamp
        # and fake gitclone-lastrun.txt, which cascades warm rebuilds and can
        # adopt a wrong-pin source. The guard flip must leave the else-branch
        # (and everything else) intact.
        self.assertIn("check-git", fixed)  # step text remains, dead
        self.assertEqual(len(text.splitlines()), len(fixed.splitlines()))
        self.assertEqual(pin_packages.neutralize_check_git(fixed), fixed)

    def test_neutralize_check_git_fails_on_unknown_shape(self):
        with self.assertRaises(SystemExit):
            pin_packages.neutralize_check_git("function(force_rebuild_git _name)\n")

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
