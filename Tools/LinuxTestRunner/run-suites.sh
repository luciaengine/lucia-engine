#!/bin/bash
# Run from the configured build directory.
set -u
O=$1
runner=$(dirname "$0")
gate_status=0
suites=()
for t in bin/DebugFast/Tests/*; do
  [ -x "$t" ] && [ -f "$t" ] || continue
  n=$(basename "$t")
  case "$n" in *.so|*.so.*|FakeAgentCli|ProcessStreamProbe) continue;; *Test*) ;; *) continue;; esac
  suites+=("$PWD/$t")
  GE_VK_VALIDATION=1 timeout 2400 "./$t" "--gtest_output=xml:$O/xml/$n.xml" > "$O/$n.log" 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    python3 "$runner/check-gtest.py" "$O/xml/$n.xml" >> "$O/$n.log" 2>&1
    rc=$?
  fi
  [ "$rc" -eq 0 ] || gate_status=1
  echo "run $n $rc" >> "$O/status"
done
if [ "${#suites[@]}" -eq 0 ]; then
  echo "No test suites ran" >> "$O/status"
  gate_status=1
fi
GE_VK_VALIDATION=1 python3 "$runner/run-ctest.py" "$O" "${suites[@]}" > "$O/ctest.log" 2>&1
rc=$?
echo "ctest $rc" >> "$O/status"
[ "$rc" -eq 0 ] || gate_status=1
echo done >> "$O/status"
exit "$gate_status"
