#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
task_binary=$(mktemp "${TMPDIR:-/tmp}/qidi-mcp-protocol.XXXXXX")
trap 'rm -f "$task_binary"' EXIT HUP INT TERM
"${CXX:-/usr/bin/clang++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -I"$task_root/src" "$task_root/tools/mcp/protocol-tests.cpp" -o "$task_binary"
"$task_binary"
