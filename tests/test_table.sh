#!/usr/bin/env bash
# test_table.sh -- AHDI parse guards: both byte orders, 5-partition reject,
# MBR-signature warning, capacity guard. Uses the built binary only.
set -u
QXL="${1:?qxltool path}" ; PY="${2:?python}"
DIR=$(mktemp -d); trap 'rm -rf "$DIR"' EXIT
cd "$DIR"

for i in 1 2 3 4 5; do "$QXL" -w p$i.win 1 P$i >/dev/null 2>&1; done

# 1. >4 partitions must be rejected.
if "$QXL" cardbuild p1.win p2.win p3.win p4.win p5.win -o five.img >/dev/null 2>&1; then
  echo "FAIL: >4 partitions accepted"; exit 1
fi

# 2. Capacity: partitions exceed card size must be rejected.
if "$QXL" cardbuild p1.win p2.win -o cap.img --card-sectors 3000 >/dev/null 2>&1; then
  echo "FAIL: over-capacity build accepted"; exit 1
fi

# 3. Build a normal card and stamp an MBR signature at 0x1FE (both orders).
"$QXL" cardbuild p1.win -o m.img >/dev/null 2>&1
# (a) raw (swapped) card: stamp BE signature; parser must warn.
"$PY" - "$DIR" <<'EOF'
import sys
d=sys.argv[1]
p=d+'/m.img'
b=bytearray(open(p,'rb').read(512))
b[0x1fe]=0x55; b[0x1ff]=0xAA
open(p,'wb').write(bytes(b))
EOF
"$QXL" cardinfo m.img > o.txt 2>&1
grep -qi "MBR signature" o.txt || { echo "FAIL: no MBR warning on swapped card"; cat o.txt; exit 1; }

# (b) a big-endian (unswapped, --no-table-swap) card: verify parse succeeds.
"$QXL" cardbuild p1.win -o m2.img --no-table-swap >/dev/null 2>&1
"$PY" - "$DIR" <<'EOF'
import sys
d=sys.argv[1]; p=d+'/m2.img'
b=bytearray(open(p,'rb').read(512))
b[0x1fe]=0x55; b[0x1ff]=0xAA
open(p,'wb').write(bytes(b))
EOF
"$QXL" cardinfo m2.img > o2.txt 2>&1
grep -q "partition table order: big-endian" o2.txt || { echo "FAIL: big-endian card not parsed"; cat o2.txt; exit 1; }
grep -qi "MBR signature" o2.txt || { echo "FAIL: no MBR warning on big-endian card"; cat o2.txt; exit 1; }

echo "table ok"
