#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

case "${1:-debug}" in
debug)
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build
    ;;
release)
    cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release
    ;;
run) # ./build.sh run [map-folder]
    "$0" debug
    shift
    ./build/tanuki-notes "$@"
    ;;
test)
    "$0" debug
    ./build/tanuki-notes --selftest
    ;;
clean)
    rm -rf build build-release
    ;;
*)
    echo "usage: $0 [debug|release|run [map-folder]|test|clean]" >&2
    exit 1
    ;;
esac
