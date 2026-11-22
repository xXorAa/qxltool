#!/usr/bin/env bash
# test_partition_open.sh -- Phase E: open a card partition interactively with
# the card.img#N form, read files AND write files through the byte-swap.
set -u
QXL="${1:?qxltool path}" ; PY="${2:?python}"
DIR=$(mktemp -d); trap 'rm -rf "$DIR"' EXIT
cd "$DIR"

# Build a card from an empty .WIN.
"$QXL" -w a.win 2 ALPHA >/dev/null 2>&1
"$QXL" cardbuild a.win -o card.img >/dev/null 2>&1

# (a) WRITE a file directly into partition #1 (read-write) and verify it
#     persists by listing and by extracting the partition and searching.
printf 'partition-write-payload\n' > payload.txt
out=$("$QXL" -w -c "wr payload.txt" "card.img#1" 2>&1)
echo "$out" | grep -qi "read only\|invalid" && { echo "FAIL: write reported problem"; echo "$out"; exit 1; }

# list must show it
lsout=$("$QXL" -c "ls" "card.img#1" 2>&1)
echo "$lsout" | grep -q "payload_txt" || { echo "FAIL: partition ls did not show payload_txt"; echo "$lsout"; exit 1; }

# cat must return the text through the byte-swap
catout=$("$QXL" -c "cat payload_txt" "card.img#1" 2>&1)
echo "$catout" | grep -q "partition-write-payload" || { echo "FAIL: cat text mismatch"; echo "$catout"; exit 1; }

# Extract the partition and confirm the bytes are actually on disk.
"$QXL" cardextract card.img --partition 1 -o p1.win >/dev/null 2>&1
"$PY" - "$DIR" <<'EOF'
import sys
b=open(sys.argv[1]+'/p1.win','rb').read()
assert b.find(b'partition-write-payload') >= 0, "write did not persist to disk"
sys.exit(0)
EOF

# (b) A missing slot must fail gracefully (exit non-zero).
if "$QXL" -c "ls" "card.img#9" >/dev/null 2>&1; then
  echo "FAIL: slot 9 should not open"; exit 1
fi

echo "partition_open ok"
