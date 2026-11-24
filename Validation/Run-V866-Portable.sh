#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${CXX:=g++}"
mkdir -p /tmp/filesync_v866_layout
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic -O1 Validation/PortableServerDialogLayout.cpp -o /tmp/filesync_v866_layout/layout
/tmp/filesync_v866_layout/layout
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic -O1 Validation/PortableServerLimits.cpp -o /tmp/filesync_v866_layout/limits
/tmp/filesync_v866_layout/limits
for test in test_v86_ui test_v84_adaptive test_v8_queue; do
  "$CXX" -std=c++17 -Wall -Wextra -Werror -pthread -O1 "Tests/Portable/${test}.cpp" -o "/tmp/filesync_v866_layout/${test}"
  "/tmp/filesync_v866_layout/${test}"
done
