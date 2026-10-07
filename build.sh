#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

mkdir -p dist

echo "=== Compiling Windows Resources (Keycap Icon) ==="
zig rc -i src -i . src/resources.rc dist/resources.res

echo "=== Building KeyIndicator (Windows x86_64 GUI) ==="
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
    dist/resources.res \
    -luser32 -lgdi32 -lshell32 -lshlwapi -ladvapi32 \
    -o dist/KeyIndicator.exe

echo "Build successful: dist/KeyIndicator.exe"
ls -lh dist/KeyIndicator.exe
