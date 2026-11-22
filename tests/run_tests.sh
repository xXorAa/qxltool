#!/usr/bin/env bash
# tests/run_tests.sh  -- run all tests, fail loudly on any failure
set -u
cd "$(dirname "$0")"
QXL="$(cd ../Unix && pwd)/qxltool"
PY=python3
fail=0
for t in test_*.sh; do
  [ -f "$t" ] || continue
  echo "== $t =="
  if bash "$t" "$QXL" "$PY"; then echo "PASS $t"; else echo "FAIL $t"; fail=1; fi
done
echo
[ "$fail" = 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $fail
