#!/usr/bin/env bash
# test_extract_dir.sh -- cardextract must create the output directory itself.
set -u
QXL="${1:?qxltool path}" ; PY="${2:?python}"
DIR=$(mktemp -d); trap 'rm -rf "$DIR"' EXIT
cd "$DIR"

"$QXL" -w a.win 1 ALPHA >/dev/null 2>&1
"$QXL" cardbuild a.win -o card.img >/dev/null 2>&1

# Extract to a directory that does not exist yet.
"$QXL" cardextract card.img -d out_nonexistent >/dev/null 2>&1 || { echo "FAIL: extract into new dir failed"; exit 1; }
[ -f out_nonexistent/part1.win ] || { echo "FAIL: out_nonexistent/part1.win missing"; exit 1; }

echo "extract_dir ok"
