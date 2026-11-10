#!/bin/sh
# Build the library and the test programs for Windows with mingw-w64 and run
# them under Wine, to try the Windows file and mapping code on a Unix system.
#
# Usage: tests/cross-windows.sh [64|32] [workdir]
#
# It needs g++-mingw-w64-x86-64 (or -i686) and wine (for 32: wine32 as well).
# The programs are linked statically, so there is no library to find.  This
# runs regress, twide, tfuzz, and tlarge with a datafile of over 4 Gb (so it
# needs the disk space and some time), see tlarge.cpp for what it does.
#
# This does not try the Microsoft compiler: the project files in win/ are
# not used, nor is _fseeki64, which is what that compiler gets.

set -e
BITS=${1:-64}
WORK=${2:-./cross-windows-$BITS}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [ "$BITS" = 32 ]; then
  CXX=i686-w64-mingw32-g++
  TLARGE_ARGS="18 256 --expect-all --unmapped --ints=40" # fits in 2 Gb
  export WINEARCH=win32
else
  CXX=x86_64-w64-mingw32-g++
  TLARGE_ARGS="8 512 --expect-all"
fi

mkdir -p "$WORK/obj" "$WORK/run/tests"
cd "$WORK"
export WINEPREFIX="$PWD/prefix" WINEDEBUG=-all DISPLAY=
FLAGS="-O2 -static -I$ROOT/include -I$ROOT/src -I$ROOT/win"

OBJS=
for f in column custom derived field fileio format handler persist remap std \
  store string table univ view viewx; do
  $CXX $FLAGS -c "$ROOT/src/$f.cpp" -o "obj/$f.o"
  OBJS="$OBJS obj/$f.o"
done

T=$ROOT/tests
$CXX $FLAGS -o regress.exe $T/regress.cpp $T/tbasic1.cpp $T/tbasic2.cpp \
  $T/tcusto1.cpp $T/tcusto2.cpp $T/tdiffer.cpp $T/textend.cpp $T/tformat.cpp \
  $T/tlimits.cpp $T/tmapped.cpp $T/tnotify.cpp $T/tresize.cpp $T/tstore1.cpp \
  $T/tstore2.cpp $T/tstore3.cpp $T/tstore4.cpp $T/tstore5.cpp $OBJS
$CXX $FLAGS -o twide.exe $T/twide.cpp $OBJS
$CXX $FLAGS -o tfuzz.exe $T/tfuzz.cpp $OBJS
$CXX $FLAGS -o tcrash.exe $T/tcrash.cpp $OBJS
$CXX $FLAGS -o tlock.exe $T/tlock.cpp $OBJS
$CXX $FLAGS -o tlarge.exe $T/tlarge.cpp $OBJS

cp "$ROOT/unix/reversed" run/
cd run
echo "== regress"
wine ../regress.exe > /dev/null
diff -r --strip-trailing-cr "$ROOT/tests/ok" tests && echo "regress output is as expected"
echo "== twide"
wine ../twide.exe | tail -1
echo "== tfuzz"
wine ../tfuzz.exe | tail -1
echo "== tcrash"
wine ../tcrash.exe | tail -1
echo "== tlock"
wine ../tlock.exe | tail -1
echo "== tlarge"
wine ../tlarge.exe tlarge.mk $TLARGE_ARGS | tail -1
rm -f tlarge.mk tlarge_ints.mk
