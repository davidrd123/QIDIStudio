#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
task_deps=${QIDI_DEPS_PREFIX:-"$task_root/deps/build/arm64/QIDIStudio_deps/usr/local"}
task_binary=$(mktemp "${TMPDIR:-/tmp}/qidi-local-network.XXXXXX")
trap 'rm -f "$task_binary"' EXIT HUP INT TERM
"${CXX:-/usr/bin/clang++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -I"$task_root/src" -isystem "$task_deps/include" \
    "$task_root/tools/mcp/local-network-tests.cpp" \
    "$task_root/src/slic3r/GUI/QidiLocalDeviceWebSocket.cpp" -o "$task_binary"
"$task_binary"
