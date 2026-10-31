#!/usr/bin/env bash
# Check the Swift build driver's Utility.launch (Sources/BuildScripts/
# XCFrameworkBuild/base.swift): short and large child output is captured whole
# with stderr kept apart, and a failing child's exit code is thrown. Downloads
# and builds nothing.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cat > "$work/main.swift" <<'SWIFT'
import Foundation
enum BaseBuild { static let defaultPath = "/usr/bin:/bin" }
SWIFT
python3 "$root/scripts/extract.py" range \
    "$root/Sources/BuildScripts/XCFrameworkBuild/base.swift" "$work/utility.swift" \
    --first 'enum Utility {' --last '    /// Every regular file in `directory`, recursively.'
cat "$work/utility.swift" >> "$work/main.swift"
cat >> "$work/main.swift" <<'SWIFT'
}
for _ in 0..<64 {
    let result = try Utility.launch(path: "/usr/bin/printf", arguments: ["/sdk/pkgconfig"], isOutput: true)
    precondition(result == "/sdk/pkgconfig", "short process output was lost")
}
let result = try Utility.launch(path: "/bin/bash", arguments: ["-c",
    "for ((i=0; i<8192; i++)); do printf 0123456789abcdef; printf diagnostic >&2; done"], isOutput: true)
precondition(result == String(repeating: "0123456789abcdef", count: 8192))
do {
    try Utility.launch(path: "/bin/bash", arguments: ["-c", "printf failure >&2; exit 7"])
    preconditionFailure("failed child must throw")
} catch {
    precondition((error as NSError).code == 7)
}
print("process output capture and failure propagation passed")
SWIFT
xcrun swiftc -swift-version 5 "$work/main.swift" -o "$work/test"
"$work/test" > "$work/test.log" 2>&1 || { cat "$work/test.log"; exit 1; }
tail -n 1 "$work/test.log"
