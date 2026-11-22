#!/usr/bin/env bash
# test_info_win.sh -- cardinfo on a bare .WIN: header fields + geometry-0/0 warning.
set -u
QXL="${1:?qxltool path}" ; PY="${2:?python}"
DIR=$(mktemp -d); trap 'rm -rf "$DIR"' EXIT
cd "$DIR"

# Build a tiny QLWA .WIN (geometry 0/0, QPC2/Q68 style) via qxltool format.
"$QXL" -w qxtest.win 1 TESTV >/dev/null || { echo "format failed"; exit 1; }

out=$("$QXL" cardinfo qxtest.win 2>&1)

# Must print the QLWA id and name
echo "$out" | grep -q "QLWA" || { echo "FAIL: no QLWA id in output"; echo "$out"; exit 1; }
echo "$out" | grep -qi "TESTV" || { echo "FAIL: no name TESTV"; echo "$out"; exit 1; }

# Must warn that geometry is 0/0 (the whole point of the tool)
echo "$out" | grep -q "geometry.*0/0\|0/0.*geometry\|not.*patched\|QPC2\|Q68" \
  || { echo "FAIL: no geometry 0/0 warning"; echo "$out"; exit 1; }

# Geometry fields (0x24=sctt, 0x26=trkc) must be reported as 0.
echo "$out" | grep -q "sctt[^0-9]*0\|trkc[^0-9]*0" \
  || { echo "FAIL: geometry fields not shown"; echo "$out"; exit 1; }

echo "info_win ok"
echo "---- sample ----"; echo "$out"
