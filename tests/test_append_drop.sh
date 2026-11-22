#!/usr/bin/env bash
# test_append_drop.sh -- cardappend, carddrop, cardclonetable semantics.
set -u
QXL="${1:?qxltool path}" ; PY="${2:?python}"
DIR=$(mktemp -d); trap 'rm -rf "$DIR"' EXIT
cd "$DIR"

"$QXL" -w a.win 1 ALPHA >/dev/null 2>&1
"$QXL" -w b.win 1 BRAVO >/dev/null 2>&1
"$QXL" -w c.win 1 CHARLIE >/dev/null 2>&1

# Build a 1-partition card, then append a second partition.
"$QXL" cardbuild a.win -o card.img >/dev/null 2>&1 || { echo "build failed"; exit 1; }
"$QXL" cardappend card.img --win b.win --yes >/dev/null 2>&1 || { echo "append failed"; exit 1; }

# cardinfo must now show 2 partitions.
"$QXL" cardinfo card.img > info.txt 2>&1
grep -q "partition 1 (slot 1)" info.txt || { echo "no part1"; cat info.txt; exit 1; }
grep -q "partition 2 (slot 2)" info.txt || { echo "append did not add a partition"; cat info.txt; exit 1; }

# Risk: append writes payload first, then sector 0. Verify with a too-small
# card (big enough for partition 1 only) so the write is rejected and the
# table is untouched.
"$QXL" cardbuild a.win -o small.img --card-sectors 3000 >/dev/null 2>&1
# 3000 sectors fits partition 1 (2048) but not 2048+2048; append must reject.
if "$QXL" cardappend small.img --win b.win --card-sectors 3000 --yes >/dev/null 2>&1; then
  echo "FAIL: overrun should have been rejected"; exit 1
fi
"$QXL" cardinfo small.img > s.txt 2>&1
grep -q "partition 1 (slot 1)" s.txt || { echo "table damaged after failed append"; cat s.txt; exit 1; }

# carddrop: drop slot 1 of a rebuilt 2-partition card.
"$QXL" cardbuild a.win b.win -o d.img >/dev/null 2>&1
"$QXL" carddrop d.img --partition 1 --yes >/dev/null 2>&1 || { echo "drop failed"; exit 1; }
"$QXL" cardinfo d.img > dd.txt 2>&1
# slot 1 entry zeroed -> it must be gone; slot 2 must remain.
if grep -q "slot 1)" dd.txt; then echo "FAIL: slot1 still present after drop"; cat dd.txt; exit 1; fi
grep -q "slot 2)" dd.txt || { echo "slot2 should remain"; cat dd.txt; exit 1; }

# cardclonetable: copy sector 0 to a fresh file and verify parity.
"$QXL" cardclonetable d.img clone.img --yes >/dev/null 2>&1 || { echo "clone failed"; exit 1; }
"$PY" - "$DIR" <<'EOF'
import sys
d=sys.argv[1]
a=open(d+'/d.img','rb').read(512)
b=open(d+'/clone.img','rb').read(512)
assert a==b, "cloned sector 0 differs"
sys.exit(0)
EOF
echo "append_drop_clone ok"
