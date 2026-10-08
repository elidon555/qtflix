#!/usr/bin/env bash
# Build (if needed) and run QtFlix.
set -e
cd "$(dirname "$0")"
[ -d build ] || cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
exec ./build/qtflix "$@"
