#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."
if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
    echo 'VTEnc requires a native Apple Silicon macOS shell (no Rosetta).' >&2
    exit 1
fi
xcrun --find clang++
xcrun --sdk macosx --show-sdk-path
for tool in cmake ninja; do
    if ! command -v "$tool" >/dev/null; then
        echo "Missing $tool. Run brew install cmake ninja, or activate build/tools as described in README.md." >&2
        exit 1
    fi
done
cmake --preset dev
cmake --build --preset dev
