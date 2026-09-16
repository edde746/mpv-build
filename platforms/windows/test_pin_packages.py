#!/usr/bin/env python3
"""Tests for pin_packages.py.

testdata/ holds byte-exact copies of the files the pinned mpv-winbuild-cmake
commit ships (see testdata/PROVENANCE). The end-to-end test runs the real
main() over a synthetic winbuild checkout planted from them, against this
repo's versions.json and patch series; the rest pin the pure rewrites.

Run: python3 platforms/windows/test_pin_packages.py
"""

import hashlib
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
TESTDATA = HERE / "testdata"
sys.path.insert(0, str(HERE))

import pin_packages  # noqa: E402

# Where each fixture lives inside a winbuild checkout, and which component it
# carries, both derived from the driver's own tables so a new pinned package
# cannot leave the test covering a stale subset. mbedtls.cmake is upstream's
# own pinned+patched idiom: the tests read it, nothing rewrites it.
CHECKOUT_FILES = {f"{c}.cmake": f"packages/{c}.cmake" for c in pin_packages.COMPONENTS}
CHECKOUT_FILES.update({Path(r).name: r for r in pin_packages.EXTRA_COMPONENTS.values()})
CHECKOUT_FILES["custom_steps.cmake"] = "cmake/custom_steps.cmake"
COMPONENT_FILES = {c: f"{c}.cmake" for c in pin_packages.COMPONENTS}
COMPONENT_FILES.update({c: Path(r).name for c, r in pin_packages.EXTRA_COMPONENTS.items()})
IDIOM_FIXTURE = "mbedtls.cmake"

PATCH = """diff --git a/a.c b/a.c
index 0000000..1111111 100644
--- a/a.c
+++ b/a.c
@@ -1 +1 @@
-old
+new
"""


def first_word(line):
    return line.strip().split(" ", 1)[0] if line.strip() else ""


def keyword_sequence(text, keywords):
    """The keyword of each line whose first word is in `keywords`, in order."""
    return [word for word in map(first_word, text.splitlines()) if word in keywords]


def passthrough(text):
    """The lines the rewrite must leave alone: everything but the keywords it
    owns and the single GIT_REPOSITORY it retargets."""
    owned = set(pin_packages.INJECTED_KEYWORDS) | {"GIT_REPOSITORY"}
    return [line for line in text.splitlines() if first_word(line) not in owned]


def series_entries(component):
    """The repo's resolved windows series, read independently of the driver:
    series.common first, then series.<group>, comments and blanks ignored."""
    entries = []
    for name in ("series.common", f"series.{pin_packages.GROUP}"):
        path = REPO / "patches" / component / name
        if path.is_file():
            entries += [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
                        if line.strip() and not line.startswith("#")]
    return entries


def read_tree(root):
    return {str(path.relative_to(root)): path.read_bytes() for path in root.rglob("*")
            if path.is_file()}


class PinPackagesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="pin-packages-test-"))
        self.addCleanup(shutil.rmtree, self.tmp)
        versions = json.loads((REPO / "versions.json").read_text(encoding="utf-8"))
        self.pins = {component: pin_packages.resolved_pins(versions, component)
                     for component in COMPONENT_FILES}
        self.winbuild = self.tmp / "mpv-winbuild-cmake"
        for fixture, relative in CHECKOUT_FILES.items():
            path = self.winbuild / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(TESTDATA / fixture, path)

    def package_text(self, component):
        return (self.winbuild / CHECKOUT_FILES[COMPONENT_FILES[component]]).read_text(
            encoding="utf-8")

    def run_main(self):
        return pin_packages.main(["--winbuild", str(self.winbuild), "--repo", str(REPO)])

    def test_fixtures_are_the_audited_upstream_bytes(self):
        # The strip-before-inject idempotency contract rests on these being
        # upstream's own bytes, so the PROVENANCE digests are enforced rather
        # than decorative.
        recorded = {}
        for line in (TESTDATA / "PROVENANCE").read_text(encoding="utf-8").splitlines():
            fields = line.strip().lstrip("#").split()
            if len(fields) == 2 and len(fields[0]) == 64:
                recorded[fields[1]] = fields[0]
        self.assertEqual(sorted(recorded), sorted([*CHECKOUT_FILES, IDIOM_FIXTURE]))
        for name, digest in recorded.items():
            with self.subTest(fixture=name):
                blob = (TESTDATA / name).read_bytes()
                self.assertEqual(hashlib.sha256(blob).hexdigest(), digest)
        for name in CHECKOUT_FILES:
            # Nothing this script injects is already in the file: a GIT_RESET
            # or PATCH_COMMAND would survive a strip and skew the rewrite
            # (llvm carries only upstream's own GIT_REMOTE_NAME/GIT_TAG, which
            # the strip removes; mbedtls is the idiom, not a rewritten file).
            words = keyword_sequence((TESTDATA / name).read_text(encoding="utf-8"),
                                     set(pin_packages.INJECTED_KEYWORDS))
            self.assertNotIn("GIT_RESET", words)
            self.assertNotIn("PATCH_COMMAND", words)

    def test_rewrite_pins_every_component_to_its_resolved_commit(self):
        for component, fixture in COMPONENT_FILES.items():
            with self.subTest(component=component):
                pins = self.pins[component]
                text = (TESTDATA / fixture).read_text(encoding="utf-8")
                pinned = pin_packages.rewrite(text, component, pins, False)
                # GIT_TAG is the resolved commit, not the human ref: the tag
                # value is what lands in <pkg>-gitinfo.txt, the only graph
                # input that dirties the download step on a warm tree.
                self.assertIn(
                    '    UPDATE_COMMAND ""\n'
                    "    GIT_REMOTE_NAME origin\n"
                    f"    GIT_TAG {pins['commit']}\n"
                    f"    GIT_RESET {pins['commit']} #",
                    pinned,
                )
                self.assertNotIn("PATCH_COMMAND", pinned)
                repository = [line for line in pinned.splitlines() if "GIT_REPOSITORY" in line]
                self.assertEqual(len(repository), 1)
                self.assertIn(pins["url"], repository[0])
                self.assertTrue(repository[0].rstrip().endswith(".git"))
                # Everything but the pin block and GIT_REPOSITORY survives,
                # and a re-run over the output converges.
                self.assertEqual(passthrough(text), passthrough(pinned))
                self.assertEqual(pin_packages.rewrite(pinned, component, pins, False), pinned)

    def test_patched_block_follows_the_mbedtls_idiom(self):
        # The keyword shape upstream itself uses for a pinned+patched package,
        # taken from the mbedtls fixture rather than hardcoded here.
        idiom = {"PATCH_COMMAND", "UPDATE_COMMAND", "GIT_REMOTE_NAME", "GIT_TAG", "GIT_RESET"}
        pins = self.pins["mpv"]
        pinned = pin_packages.rewrite(
            (TESTDATA / "mpv.cmake").read_text(encoding="utf-8"), "mpv", pins, True)
        self.assertEqual(
            keyword_sequence(pinned, idiom),
            keyword_sequence((TESTDATA / IDIOM_FIXTURE).read_text(encoding="utf-8"), idiom),
        )
        # The patch step resets to the pin before applying: a step re-run on
        # its own (series-only change, or a warm-cache step cascade) must
        # converge instead of double-applying onto a patched tree.
        self.assertIn(
            "    PATCH_COMMAND ${EXEC} "
            f'"git reset --hard {pins["commit"]} -q '
            '&& git apply ${CMAKE_CURRENT_SOURCE_DIR}/mpv-*.patch"\n'
            '    UPDATE_COMMAND ""\n'
            "    GIT_REMOTE_NAME origin\n"
            f"    GIT_TAG {pins['commit']}\n"
            f"    GIT_RESET {pins['commit']} # {pins['version']}\n",
            pinned,
        )

    def test_main_rewrites_the_checkout_end_to_end(self):
        # main() is what CI runs: drive it instead of re-composing its stages,
        # so a dropped gate, a skipped EXTRA_COMPONENTS loop or an
        # unconditional write fails here.
        self.assertEqual(self.run_main(), 0)
        for component in COMPONENT_FILES:
            with self.subTest(component=component):
                pinned = self.package_text(component)
                self.assertIn(f"    GIT_TAG {self.pins[component]['commit']}\n", pinned)
                self.assertEqual(pinned.count("GIT_TAG"), 1)
        for component in pin_packages.COMPONENTS:
            with self.subTest(staged=component):
                entries = series_entries(component)
                staged = sorted(path.name for path
                                in (self.winbuild / "packages").glob(f"{component}-*.patch"))
                self.assertEqual(
                    staged,
                    sorted(f"{component}-{index:04d}-{name}"
                           for index, name in enumerate(entries, start=1)),
                )
                self.assertEqual("PATCH_COMMAND" in self.package_text(component), bool(entries))
        for component in pin_packages.EXTRA_COMPONENTS:
            self.assertNotIn("PATCH_COMMAND", self.package_text(component))
        # The two gates main() applies on top of the rewrite: dropping either
        # call ships a configure failure (ffmpeg aarch64 cuda) or a silently
        # disabled feature (vapoursynth).
        self.assertNotIn("-Dsubrandr=enabled", self.package_text("mpv"))
        self.assertIn("-Dvapoursynth=disabled", self.package_text("mpv"))
        self.assertNotIn(pin_packages.FFMPEG_CUDA_ORIGINAL, self.package_text("ffmpeg"))
        self.assertTrue(self.package_text("ffmpeg").startswith(pin_packages.FFMPEG_CUDA_GUARD))
        steps = (self.winbuild / "cmake/custom_steps.cmake").read_text(encoding="utf-8")
        self.assertNotIn(pin_packages.CHECK_GIT_ORIGINAL, steps)
        self.assertIn(pin_packages.CHECK_GIT_NEUTRALIZED, steps)

    def test_main_converges_on_a_second_run(self):
        self.run_main()
        first = read_tree(self.winbuild)
        # A patch left behind by a longer series must not survive the re-run:
        # the staged-patch glob is what makes the rewrite idempotent.
        (self.winbuild / "packages" / "mpv-9999-stale.patch").write_text(PATCH)
        self.assertEqual(self.run_main(), 0)
        self.assertEqual(first, read_tree(self.winbuild))

    def test_check_git_fixture_matches_audited_idiom(self):
        # neutralize_check_git string-matches the exact upstream injection
        # guard; a winbuild bump that reshapes it must fail loud, not
        # silently skip.
        text = (TESTDATA / "custom_steps.cmake").read_text(encoding="utf-8")
        self.assertEqual(text.count(pin_packages.CHECK_GIT_ORIGINAL), 1)
        self.assertNotIn(pin_packages.CHECK_GIT_NEUTRALIZED, text)

    def test_ffmpeg_cuda_is_arch_gated(self):
        # The pinned release ffmpeg's ffnvcodec probe fails on
        # aarch64-w64-mingw32; the unconditional cuda enables become a
        # variable that is only set off-aarch64.
        text = (TESTDATA / "ffmpeg.cmake").read_text(encoding="utf-8")
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
        text = (TESTDATA / "mpv.cmake").read_text(encoding="utf-8")
        self.assertIn("-Dsubrandr=enabled", text)
        self.assertIn("-Dlibcurl=enabled", text)
        self.assertIn("-Dvapoursynth=enabled", text)
        gated = pin_packages.gate_mpv_options(text)
        self.assertNotIn("-Dsubrandr", gated)
        self.assertNotIn("-Dlibcurl", gated)
        # vapoursynth flips to disabled: its import library does not satisfy
        # the pinned mpv's dllimport getVSScriptAPI reference.
        self.assertIn("-Dvapoursynth=disabled", gated)
        self.assertNotIn("-Dvapoursynth=enabled", gated)
        # Only the two master-only option lines vanish; DEPENDS entries stay.
        self.assertIn("subrandr\n", gated)
        self.assertIn("vapoursynth\n", gated)
        self.assertEqual(len(text.splitlines()) - 2, len(gated.splitlines()))
        self.assertEqual(pin_packages.gate_mpv_options(gated), gated)

    def test_neutralize_check_git_suppresses_and_converges(self):
        text = (TESTDATA / "custom_steps.cmake").read_text(encoding="utf-8")
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
