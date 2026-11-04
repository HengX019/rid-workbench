#!/bin/sh
set -eu
RID_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RID_TMP=$(mktemp -d)
trap 'rm -rf "$RID_TMP"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -I"$RID_ROOT/RID_Workbench" -c "$RID_ROOT/RID_Workbench/opendroneid.c" -o "$RID_TMP/opendroneid.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$RID_ROOT/RID_Workbench" "$RID_ROOT/tests/telemetry_test.cpp" "$RID_TMP/opendroneid.o" -o "$RID_TMP/telemetry_test"
"$RID_TMP/telemetry_test"
