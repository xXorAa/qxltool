#!/usr/bin/env bash
# test_roundtrip.sh -- cardbuild + cardextract must round-trip a partition
# byte-exactly except for the geometry-patch fields (sctt/trkc/cyld/fsct).
set -u
QXL="${1:?qxltool path}" ; PY="${2:?python}"
DIR=$(mktemp -d); trap 'rm -rf "$DIR"' EXIT
cd "$DIR"

# Two tiny QLWA .WIN fixtures (geometry 0/0, QPC2 style)
"$QXL" -w a.win 1 ALPHA >/dev/null 2>&1 || { echo "format a failed"; exit 1; }
"$QXL" -w b.win 1 BRAVO >/dev/null 2>&1 || { echo "format b failed"; exit 1; }

# Build a 2-partition card
"$QXL" cardbuild a.win b.win -o card.img >/dev/null 2>&1 || { echo "cardbuild failed"; exit 1; }

# Extract both partitions back
"$QXL" cardextract card.img -d . >/dev/null 2>&1 || { echo "cardextract failed"; exit 1; }
[ -f part1.win ] && [ -f part2.win ] || { echo "missing extracted partitions"; exit 1; }

# Assert the geometry field offsets are the ONLY differences vs the source.
"$PY" - "$DIR" <<'EOF'
import sys
d=sys.argv[1]
def load(n): return open(d+'/'+n,'rb').read()
src=load('a.win'); ext=load('part1.win')
assert len(src)==len(ext), "size mismatch %d vs %d"%(len(src),len(ext))
# Allowed differences: geometry patch region only.
# sctt@0x24,trkc@0x26,cyld@0x28 (2 bytes each) and fsct@0x3a (4 bytes).
allowed=set()
for off in (0x24,0x26,0x28): allowed |= {off, off+1}
allowed |= {0x3a,0x3b,0x3c,0x3d}
diffs=[i for i in range(len(src)) if src[i]!=ext[i]]
bad=[i for i in diffs if i not in allowed]
assert not bad, "unexpected non-geometry diffs at %s"%( [hex(x) for x in bad[:20] ] )
# Confirm the geometry fields actually changed (proof the patch ran).
assert ext[0x24]!=src[0x24] or ext[0x26]!=src[0x26] or ext[0x28]!=src[0x28], \
    "geometry fields did not change after build"
print("roundtrip ok: only geometry fields differ (diffs=%d)"%len(diffs))
sys.exit(0)
EOF
echo "roundtrip ok"
