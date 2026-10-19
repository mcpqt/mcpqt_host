#!/bin/sh
# Build the project using the existing `build` directory. Extra arguments are
# passed straight through to `cmake --build` (e.g. --target foo, -j4).
set -eu

cd "$(dirname "$0")"

cmake --build build "$@"
