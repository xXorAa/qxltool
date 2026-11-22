#!/usr/bin/env bash
# test_partition_open.sh -- Phase E: open a card partition interactively with
# the card.img#N form and read files through the byte-swap.
set -u
QXL="${1:?qxltool path}" ; PY="${2:?python}"
DIR=$(mktemp -d); trap 'rm -rf "$DIR"' EXIT
cd "$DIR"

# Build a card with a small .WIN that contains a labelled file.
"$QXL" -w a.win 1 ALPHA >/dev/null 2>&1
printf 'partition-hello-world\n' > src.txt
"$QXL" -w -c "wr src.txt" a.win >/dev/null 2>&1
"$QXL" cardbuild a.win -o card.img >/dev/null 2>&1

# Open partition #1 interactively; listing must show the file.
out=$("$QXL" -c "ls" "card.img#1" 2>&1)
echo "$out" | grep -q "src_txt" || { echo "FAIL: partition ls did not show src_txt"; echo "$out"; exit 1; }

# cat must return the original text through the byte-swap.
catout=$("$QXL" -c "cat src_txt" "card.img#1" 2>&1)
echo "$catout" | grep -q "partition-hello-world" || { echo "FAIL: cat text mismatch"; echo "$catout"; exit 1; }

# A missing slot must fail gracefully (exit non-zero).
if "$QXL" -c "ls" "card.img#9" >/dev/null 2>&1; then
  echo "FAIL: slot 9 should not open"; exit 1
fi

echo "partition_open ok"
