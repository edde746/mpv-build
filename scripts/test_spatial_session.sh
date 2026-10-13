#!/usr/bin/env bash
# Exercise the production session lease without an audio device or media.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 "$root/scripts/extract.py" post-image \
    "$root/patches/mpv/pool/0039-ao-spatial-render-truehd-atmos-and-dts-x.patch" "$work/source.swift"
python3 "$root/scripts/extract.py" range "$work/source.swift" "$work/session.swift" \
    --first 'private enum SpatialSession {' --last '@_cdecl("mp_spatial_create")'
cat "$root/scripts/test_spatial_session.swift" > "$work/main.swift"
# Compile the mobile branch on the macOS CI host against the fake session.
sed 's/#if os(iOS) || os(tvOS)/#if true/' "$work/session.swift" >> "$work/main.swift"
cat >> "$work/main.swift" <<'SWIFT'
let session = AVAudioSession.shared
func checkRestored() {
    precondition(session.category == .ambient)
    precondition(session.mode == .default)
    precondition(session.categoryOptions == [.mixWithOthers])
    precondition(!session.supportsMultichannelContent)
}
for initiallyActive in [true, false] {
    session.active = initiallyActive
    session.activationCalls = 0
    session.categoryCalls = 0
    try SpatialSession.acquire()
    precondition(session.active && session.activationCalls == 1)
    precondition(session.category == .playback && session.mode == .moviePlayback)
    precondition(session.supportsMultichannelContent)
    try SpatialSession.acquire()
    precondition(session.activationCalls == 1 && session.categoryCalls == 1)
    SpatialSession.release()
    precondition(session.active && session.category == .playback && session.categoryCalls == 1)
    SpatialSession.release()
    precondition(session.active && session.deactivationCalls == 0)
    precondition(session.categoryCalls == 2)
    checkRestored()
}
session.active = false
session.refuseActivation = true
do {
    try SpatialSession.acquire()
    preconditionFailure("activation refusal must propagate")
} catch AVAudioSession.Failure.activation {}
precondition(!session.active && session.deactivationCalls == 0)
checkRestored()
session.refuseActivation = false
try SpatialSession.acquire()
SpatialSession.release()
checkRestored() // a failed acquire must not leak a lease
precondition(session.active && session.deactivationCalls == 0)
print("session ownership, overlapping leases and failed-open rollback passed")
SWIFT
xcrun swiftc -swift-version 6 -target "$(uname -m)-apple-macos26.0" \
    "$work/main.swift" -o "$work/test"
"$work/test"
