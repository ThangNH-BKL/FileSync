#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for CXX in g++ clang++; do
  echo "Testing using $CXX"
  "$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic -O2 Validation/PortableServerLimits.cpp -o "/tmp/filesync_v865_limits_${CXX//+/p}"
  "/tmp/filesync_v865_limits_${CXX//+/p}"
done
