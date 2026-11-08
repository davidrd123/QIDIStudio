#!/bin/sh
set -eu

task_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
task_phase=${1:-all}
task_jobs=${QIDI_BUILD_JOBS:-4}
case "$task_phase" in all|deps|app) ;; *) echo 'Usage: sh tools/mcp/build-macos.sh [all|deps|app]' >&2; exit 2 ;; esac
case "$task_jobs" in ''|*[!0-9]*|0) echo 'QIDI_BUILD_JOBS must be a positive integer' >&2; exit 2 ;; esac
if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
    echo 'This development build script targets Apple Silicon macOS.' >&2
    exit 2
fi

PATH="$task_root/.build-tools/bin:$PATH"
export PATH
CMAKE_BUILD_PARALLEL_LEVEL=$task_jobs
export CMAKE_BUILD_PARALLEL_LEVEL
command -v cmake >/dev/null
command -v ninja >/dev/null
task_deps="$task_root/deps/build/arm64"
task_prefix="$task_deps/QIDIStudio_deps/usr/local"
task_build="$task_root/build/arm64-mcp"

if [ "$task_phase" = all ] || [ "$task_phase" = deps ]; then
    cmake -S "$task_root/deps" -B "$task_deps" -G Ninja \
        -DDESTDIR="$task_deps/QIDIStudio_deps" \
        -DOPENSSL_ARCH=darwin64-arm64-cc \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
    cmake --build "$task_deps" --target deps --parallel 1
fi

if [ "$task_phase" = all ] || [ "$task_phase" = app ]; then
    sh "$task_root/tools/mcp/run-protocol-tests.sh"
    QIDI_DEPS_PREFIX="$task_prefix" sh "$task_root/tools/mcp/run-local-network-tests.sh"
    cmake -S "$task_root" -B "$task_build" -G Ninja \
        -DCMAKE_PREFIX_PATH="$task_prefix" \
        -DCMAKE_INSTALL_PREFIX="$task_build/QIDIStudio" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_MACOSX_BUNDLE=ON \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
        -DQDT_RELEASE_TO_PUBLIC=0 \
        -DQDT_INTERNAL_TESTING=1 \
        -DQIDI_ENABLE_MCP=ON
    cmake --build "$task_build" --parallel "$task_jobs"
fi
