#!/bin/sh
# Syntax-check a QtTest test translation unit without a full CMake build.
# Usage: tests/qt_check.sh <path/to/test.cpp>
#
# Runs moc to produce the "<name>.moc" that an AUTOMOC-built file includes,
# then compiles the translation unit with -fsyntax-only.
set -e

f="$1"
if [ -z "$f" ]; then
    echo "usage: $0 <test.cpp>" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
QT=/usr/include/x86_64-linux-gnu/qt6
MOC=/usr/lib/qt6/libexec/moc
base=$(basename "$f" .cpp)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$MOC" "$f" -o "$tmp/$base.moc"

exec g++ -std=c++20 -fsyntax-only -fPIC \
    -I"$root/include" -I"$root/tests" -I"$tmp" \
    -I"$QT" -I"$QT/QtCore" -I"$QT/QtNetwork" -I"$QT/QtHttpServer" -I"$QT/QtTest" \
    -DMCP_ENABLE_HTTP=1 "$f"
