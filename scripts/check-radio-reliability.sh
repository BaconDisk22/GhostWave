#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../firmware"
cmake -S . -B build -DGHOSTWAVE_BUILD_VIRT=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./pi/tools/vmesh.sh build/ghostwaved
printf 'Host tests and virtual-mesh smoke completed. Physical radio/reconnect testing is still required.\n'
