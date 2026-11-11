#!/usr/bin/env bash
# macos-add-arm64.sh <app-bundle-path-under-build> [cmake args...]
# The Tahoe runner is an Intel VM, so the app is built twice: x86_64 in build/ (tested there), then
# arm64 here in build-arm64/ (cross-compiled; it cannot run). Every Mach-O in build/'s bundle that
# is not already universal is then lipo'd with its arm64 counterpart, giving one universal bundle.
# Dependencies that are built universal once (the FujiNet runtime, SDL3, OpenSSL) are shared.
# Optional environment: ARM64_PRE, a shell snippet run first (e.g. to move aside a work directory the
# x86_64 build left behind); ARM64_ENV, extra VAR=value words for the arm64 cmake runs.
set -euo pipefail
APP=$1; shift
eval "${ARM64_PRE:-true}"

env ${ARM64_ENV:-} CMAKE_OSX_ARCHITECTURES="arm64;x86_64" cmake -B build-arm64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 "$@"
env ${ARM64_ENV:-} CMAKE_OSX_ARCHITECTURES="arm64;x86_64" cmake --build build-arm64

find "build/$APP" -type f -print0 | while IFS= read -r -d '' f; do
    file "$f" | grep -q Mach-O || continue
    lipo -archs "$f" | grep -q arm64 && continue
    other="build-arm64/${f#build/}"
    [ -f "$other" ] || { echo "::error::no arm64 counterpart of ${f#build/}"; exit 1; }
    mode=$(stat -f %Lp "$f")
    lipo -create "$f" "$other" -output "$f.universal"
    chmod "$mode" "$f.universal"
    mv "$f.universal" "$f"
    echo "universal: ${f#build/} ($(lipo -archs "$f"))"
done
