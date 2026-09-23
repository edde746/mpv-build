#!/usr/bin/env bash
# Compile production timing, admission and draw/flip from the final Android
# patch series. Pass an already-patched mpv tree for offline use; otherwise
# fetch and patch the pinned source in a temporary directory.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved mpv/android series.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi

# Every region below is production code sliced verbatim; only platform, codec and
# OSD side effects are replaced by the host harness.
src="$source_dir/video/out/vo_mediacodec.c"
vo="$source_dir/video/out/vo.c"
E="$root/scripts/extract.py"

python3 "$E" region "$src" "$workdir/mediacodec_timing_core.inc" --start "mediacodec timing core"
python3 "$E" region "$src" "$workdir/mediacodec_stats_core.inc" --start "mediacodec stats core"

# Preparation, draw and flip, not a parallel scheduling model: the defines the
# driver reads, then each production function by name.
python3 "$E" range "$src" "$workdir/mediacodec_timing_driver.inc" \
    --first "#define VSYNC_SAMPLE_MAX_AGE_NS" --last "static int64_t read_vsync_sample"
for fn in read_vsync_sample read_vsync_period read_vsync_spacing request_vsync_sample \
          vsync_sample_is_fresh plausible_period mode_period update_queue_timing \
          get_release_target prepare_osd submit_deadline prepare_frame draw_frame \
          present_reports_on present_feedback flip_page reset_video; do
    python3 "$E" symbol "$src" "$workdir/mediacodec_timing_driver.inc" --append --fn "$fn"
done

python3 "$E" range "$src" "$workdir/mediacodec_timing_cadence.inc" \
    --first "#define OSD_CADENCE_SAMPLES" --last "struct osd_spec {"

python3 "$E" symbol "$vo" "$workdir/mediacodec_timing_admission.inc" \
    --fn prepare_queued_frame --fn vo_wakeup_deadline

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_mediacodec_timing.c" -lm

"$workdir/test"
