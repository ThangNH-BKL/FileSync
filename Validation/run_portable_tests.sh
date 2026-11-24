#!/usr/bin/env bash
# Non-Windows portable smoke suite, NOT a substitute for MSVC/NTFS/E2E.
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-g++}"
FLAGS=(-std=c++17 -O1 -Wall -Wextra -Werror -pthread)
mkdir -p /tmp/filesync_v8_tests
for test in test_v7_native test_v8_queue test_v8_usn_guard test_v8_index test_v8_faults test_v8_windows_max_macro test_v84_adaptive test_v85_lsm test_v85_faults test_v86_ui; do
  echo "==> $test"
  "$CXX" "${FLAGS[@]}" "Tests/Portable/$test.cpp" -o "/tmp/filesync_v8_tests/$test"
  "/tmp/filesync_v8_tests/$test"
done
