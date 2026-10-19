#!/bin/sh
# Reconfigure the CMake build the way this checkout is currently configured:
# Unix Makefiles generator, RelWithDebInfo, tests + examples + HTTP on,
# warnings-as-errors on, sanitizers/coverage off. Extra arguments are passed
# straight through to CMake.
set -eu

cd "$(dirname "$0")"

cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DMCP_BUILD_TESTS=ON \
    -DMCP_BUILD_EXAMPLES=ON \
    -DMCP_ENABLE_HTTP=ON \
    -DMCP_WARNINGS_AS_ERRORS=ON \
    -DMCP_ENABLE_ASAN=OFF \
    -DMCP_ENABLE_TSAN=OFF \
    -DMCP_ENABLE_COVERAGE=OFF \
    "$@"
