#!/usr/bin/env bash
# Run all linters. qmllint always; clang-tidy and clazy if installed
# (sudo apt install clang-tidy clazy).
set -e
cd "$(dirname "$0")"
[ -d build ] || cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build lint
