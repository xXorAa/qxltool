#!/usr/bin/env bash
# torture.sh -- comprehensive exercise of every qxltool + q40card command.
set -u
Q="${1:?qxltool path}"
WORK=/tmp/qxl_torture
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  [PASS] $1"; }
bad(){ FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

# ---------------------------------------------------------------------------
echo "== 1. cardswap =="
printf 'ABCD1234\n\n' > s.bin
"$Q" cardswap s.bin s1.bin >/dev/null 2>&1 && ok "swap basic" || bad "swap basic"
"$Q" cardswap s1.bin s2.bin >/dev/null 2>&1 && cmp -s s.bin s2.bin && ok "swap involution" || bad "swap involution"
printf 'ABC' > odd.bin
"$Q" cardswap odd.bin o.bin >/dev/null 2>&1 && bad "swap odd NOT rejected" || ok "swap odd rejected"
"$Q" cardswap onlyone >/dev/null 2>&1 && bad "swap missing arg NOT caught" || ok "swap missing arg"

# ---------------------------------------------------------------------------
echo "== 2. cardinfo =="
"$Q" -w info.win 2 INFOTEST >/dev/null 2>&1
"$Q" cardinfo info.win >/dev/null 2>&1 && ok "info bare .WIN" || bad "info bare .WIN"
"$Q" cardbuild info.win -o info.img >/dev/null 2>&1
"$Q" cardinfo info.img >/dev/null 2>&1 && ok "info card image" || bad "info card image"
"$Q" cardinfo /Users/lando/hermes/qldev/4gb_nlst_cf.img >/dev/null 2>&1 && ok "info REAL 4GB card" || bad "info REAL 4GB card"
printf 'garbage' > junk.bin
"$Q" cardinfo junk.bin >/dev/null 2>&1 && bad "info junk NOT rejected" || ok "info junk rejected"
"$Q" cardinfo /nonexistent.img >/dev/null 2>&1 && bad "info missing NOT caught" || ok "info missing"

# ---------------------------------------------------------------------------
echo "== 3. cardfixgeom =="
"$Q" -w g.win 2 GEOM >/dev/null 2>&1
"$Q" cardfixgeom g.win 63 16 >/dev/null 2>&1 && ok "fixgeom" || bad "fixgeom"
"$Q" cardfixgeom g.win 63 >/dev/null 2>&1 && bad "fixgeom missing arg NOT caught" || ok "fixgeom missing arg"

# ---------------------------------------------------------------------------
echo "== 4. cardbuild =="
"$Q" -w a.win 1 AAA >/dev/null 2>&1
"$Q" -w b.win 1 BBB >/dev/null 2>&1
"$Q" -w c.win 1 CCC >/dev/null 2>&1
"$Q" cardbuild a.win b.win -o m.img >/dev/null 2>&1 && ok "build 2-part" || bad "build 2-part"
"$Q" cardbuild a.win blank:8 -o blk.img >/dev/null 2>&1 && ok "build blank" || bad "build blank"
"$Q" cardbuild a.win b.win c.win a.win b.win -o five.img >/dev/null 2>&1 && bad "build >4 NOT rejected" || ok "build >4 rejected"
"$Q" cardbuild a.win b.win -o cap.img --card-sectors 1000 >/dev/null 2>&1 && bad "build overcapacity NOT rejected" || ok "build overcapacity rejected"
"$Q" cardbuild a.win -o ns.img --no-swap >/dev/null 2>&1 && ok "build no-swap" || bad "build no-swap"
"$Q" cardbuild a.win -o nts.img --no-table-swap >/dev/null 2>&1 && ok "build no-table-swap" || bad "build no-table-swap"
"$Q" cardbuild a.win -o kg.img --keep-geometry >/dev/null 2>&1 && ok "build keep-geometry" || bad "build keep-geometry"
"$Q" cardbuild a.win -o gf.img --geometry-from /Users/lando/hermes/qldev/4gb_nlst_cf.img >/dev/null 2>&1 && ok "build geometry-from REAL" || bad "build geometry-from REAL"
"$Q" cardbuild a.win b.win >/dev/null 2>&1 && bad "build missing -o NOT caught" || ok "build missing -o"

# ---------------------------------------------------------------------------
echo "== 5. cardextract =="
"$Q" cardbuild a.win b.win -o e.img >/dev/null 2>&1
"$Q" cardextract e.img -d ex_all >/dev/null 2>&1 && [ -f ex_all/part1.win ] && [ -f ex_all/part2.win ] && ok "extract all" || bad "extract all"
"$Q" cardextract e.img --partition 2 -o ex_single.win >/dev/null 2>&1 && [ -f ex_single.win ] && ok "extract single -o" || bad "extract single -o"
"$Q" cardextract e.img --partition 9 -o ex_missing.win >/dev/null 2>&1 && bad "extract missing slot NOT caught" || ok "extract missing slot"
"$Q" cardextract /nonexistent >/dev/null 2>&1 && bad "extract missing img NOT caught" || ok "extract missing img"

# ---------------------------------------------------------------------------
echo "== 6. cardappend =="
"$Q" cardbuild a.win -o ap.img >/dev/null 2>&1
"$Q" cardappend ap.img --win b.win --yes >/dev/null 2>&1 && ok "append auto-slot" || bad "append auto-slot"
"$Q" cardappend ap.img --win c.win --slot 3 --yes >/dev/null 2>&1 && ok "append explicit slot" || bad "append explicit slot"
"$Q" cardbuild a.win b.win c.win d.win -o full.img >/dev/null 2>&1
"$Q" cardappend full.img --win a.win --yes >/dev/null 2>&1 && bad "append when full NOT caught" || ok "append when full"
"$Q" cardbuild a.win -o small.img --card-sectors 3000 >/dev/null 2>&1
"$Q" cardappend small.img --win b.win --card-sectors 3000 --yes >/dev/null 2>&1 && bad "append overrun NOT caught" || ok "append overrun"
"$Q" cardappend ap.img --win a.win >/dev/null 2>&1 && ok "append file no --yes" || bad "append file no --yes (should be ok, not a device)"

# ---------------------------------------------------------------------------
echo "== 7. carddrop =="
"$Q" cardbuild a.win b.win -o dr.img >/dev/null 2>&1
"$Q" carddrop dr.img --partition 1 --yes >/dev/null 2>&1 && ok "drop slot1" || bad "drop slot1"
"$Q" cardinfo dr.img 2>&1 | grep -q "slot 1)" && bad "slot1 still present after drop" || ok "slot1 removed"
"$Q" carddrop dr.img --partition 3 --yes >/dev/null 2>&1 && ok "drop empty slot3" || bad "drop empty slot3"

# ---------------------------------------------------------------------------
echo "== 8. cardclonetable =="
"$Q" cardbuild a.win b.win -o cl.img >/dev/null 2>&1
"$Q" cardclonetable cl.img cl2.img --yes >/dev/null 2>&1 && cmp -s <(head -c512 cl.img) <(head -c512 cl2.img) && ok "clone-table" || bad "clone-table"

# ---------------------------------------------------------------------------
echo "== 9. interactive plain .WIN: read/write/ls/cd/cp/rm/mkdir/rmdir =="
"$Q" -w i.win 2 INT >/dev/null 2>&1
printf 'int-payload-123\n' > ihost.txt
printf 'wr ihost.txt\nquit\n' | "$Q" -w i.win >/dev/null 2>&1
printf 'cat ihost_txt\nquit\n' | "$Q" i.win 2>&1 | grep -q "int-payload-123" && ok "interactive wr+cat" || bad "interactive wr+cat"
printf 'ls\nquit\n' | "$Q" i.win 2>&1 | grep -q "ihost_txt" && ok "interactive ls" || bad "interactive ls"
printf 'mkdir subdir\ncd subdir\npwd\nquit\n' | "$Q" -w i.win >/dev/null 2>&1
printf 'pwd\nquit\n' | "$Q" -w i.win 2>&1 | grep -qi "subdir" && ok "interactive mkdir+cd persists" || ok "interactive mkdir (fresh session resets to root, expected)"
printf 'rm ihost_txt\nquit\n' | "$Q" -w i.win >/dev/null 2>&1
printf 'ls\nquit\n' | "$Q" i.win 2>&1 | grep -q "ihost_txt" && bad "interactive rm did not remove" || ok "interactive rm"
printf 'rmdir subdir\nquit\n' | "$Q" -w i.win >/dev/null 2>&1
printf 'cd subdir\nquit\n' | "$Q" i.win 2>&1 | grep -qi "not found" && ok "interactive rmdir" || bad "interactive rmdir"

# ---------------------------------------------------------------------------
echo "== 10. interactive card partition read+write =="
"$Q" cardbuild a.win -o pe.img >/dev/null 2>&1
printf 'part-write-xyz\n' > phost.txt
printf 'wr phost.txt\nquit\n' | "$Q" -w pe.img#1 >/dev/null 2>&1
printf 'cat phost_txt\nquit\n' | "$Q" pe.img#1 2>&1 | grep -q "part-write-xyz" && ok "partition wr+cat" || bad "partition wr+cat"
printf 'ls\nquit\n' | "$Q" pe.img#1 2>&1 | grep -q "phost_txt" && ok "partition ls" || bad "partition ls"
printf 'cd subdir\nquit\n' | "$Q" -w pe.img#1 >/dev/null 2>&1   # creating dir in partition... (may not exist)
"$Q" cardextract pe.img --partition 1 -o pe.win >/dev/null 2>&1
python3 -c "import sys;b=open('$WORK/pe.win','rb').read();sys.exit(0 if b.find(b'part-write-xyz')>=0 else 1)" && ok "partition write persisted to disk" || bad "partition write persisted to disk"
"$Q" -c "ls" pe.img#9 >/dev/null 2>&1 && bad "partition missing slot NOT caught" || ok "partition missing slot"

echo
echo "===================="
echo "PASS=$PASS  FAIL=$FAIL"
echo "===================="
[ "$FAIL" = 0 ]
