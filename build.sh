#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

mkdir -p dist

echo "=== Building TaskbarKeyIndicator (Windows x86_64 GUI) ==="
zig c++ -target x86_64-windows-gnu \
    -O2 \
    -Wl,--subsystem,windows \
    -std=c++17 \
    -Isrc \
    src/main.cpp \
    src/config.cpp \
    src/logger.cpp \
    src/taskbar_overlay.cpp \
    src/browser_watcher.cpp \
    -luser32 -lgdi32 -lshell32 -lshlwapi -ladvapi32 \
    -o dist/TaskbarKeyIndicator.exe

echo "Build successful: dist/TaskbarKeyIndicator.exe"
ls -lh dist/TaskbarKeyIndicator.exe
