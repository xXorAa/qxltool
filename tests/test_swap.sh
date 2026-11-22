#!/usr/bin/env bash
# test_swap.sh -- the 16-bit byte-swap primitive must be exact + involution.
set -u
QXL="${1:?qxltool path}" ; PY="${2:?python}"
DIR=$(mktemp -d); trap 'rm -rf "$DIR"' EXIT
cd "$DIR"

# 1. Known even-length file with distinct per-word patterns.
printf 'ABCD1234\n\n' > in.bin        # 12 bytes even: "ABCD1234\n\n"
# 2. Run cardswap; transpose must be exact per 16-bit word.
"$QXL" cardswap in.bin out.bin || { echo "cardswap failed"; exit 1; }
"$PY" - "$DIR" <<'EOF'
import sys, struct
d=open(sys.argv[1]+'/out.bin','rb').read()
exp=open(sys.argv[1]+'/in.bin','rb').read()
sw=bytearray(exp)
for i in range(0,len(sw),2): sw[i],sw[i+1]=sw[i+1],sw[i]
assert d==bytes(sw), "swap output mismatch"
sys.exit(0)
EOF
# 3. Involution: swap again must return the original bytes.
"$QXL" cardswap out.bin back.bin
cmp -s in.bin back.bin || { echo "involution failed"; exit 1; }

# 4. Odd-length must be rejected.
printf 'ABC' > odd.bin
if "$QXL" cardswap odd.bin oddout.bin 2>/dev/null; then
  echo "FAIL: odd length not rejected"; exit 1
fi
echo "swap ok"
