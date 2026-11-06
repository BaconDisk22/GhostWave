#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../firmware"
cmake -S . -B build -DGHOSTWAVE_BUILD_VIRT=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build build --parallel
# Repeat the fast host suite to catch nondeterministic SOS and beacon regressions.
# A single green run previously masked intermittent delivery failures.
ctest --test-dir build --output-on-failure --repeat until-fail:5
# Use a per-process multicast port to avoid collisions with parallel CI jobs.
# Explicit VMESH_PORT overrides this for reproducing a failing run.
if [[ -z "${VMESH_PORT:-}" ]]; then
  export VMESH_PORT=$(( 48000 + (BASHPID % 10000) ))
fi
echo "Virtual mesh multicast port: $VMESH_PORT"
./pi/tools/vmesh.sh build/ghostwaved
printf 'Host and virtual-mesh checks passed; physical-radio validation still required.\n'
