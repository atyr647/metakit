#!/bin/sh
# Build the Python binding (Mk4py, for Python 2) and run its tests.
#
# Usage: python/test/ci.sh [python-executable]
#
# This does what setup.py does, without distutils: it builds the library
# objects and the binding into one module, in a scratch directory, and runs
# every test module there.  Used by .github/workflows/ci.yml.

set -e
PYTHON=${1:-python}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK="${TMPDIR:-/tmp}/mk4py-ci-work"
rm -rf "$WORK"
mkdir -p "$WORK/conf" "$WORK/run"

# config.h for the library, as made for the Unix build
(cd "$WORK/conf" && "$ROOT/unix/configure" > /dev/null)

PYINC=$($PYTHON -c "import sysconfig; print(sysconfig.get_paths()['include'])")
CXX=${CXX:-g++}
# (setup.py defines this too: the binding takes Unicode strings with it)
FLAGS="-O1 -g -fPIC -w -DHAVE_UNICODEOBJECT_H"
cd "$WORK"

for f in column custom derived fileio field format handler persist remap std \
    store string table univ view viewx; do
  $CXX -c $FLAGS -I"$WORK/conf" -I"$ROOT/include" -I"$ROOT/src" \
    "$ROOT/src/$f.cpp" -o "$f.o" &
done
for f in PyProperty PyRowRef PyStorage PyView; do
  $CXX -c $FLAGS -I"$PYINC" -I"$ROOT/python/scxx" -I"$ROOT/include" \
    "$ROOT/python/$f.cpp" -o "$f.o" &
done
$CXX -c $FLAGS -I"$PYINC" -I"$ROOT/python/scxx" -I"$ROOT/include" \
  "$ROOT/python/scxx/PWOImp.cpp" -o PWOImp.o
wait

$CXX -shared -o run/Mk4py.so *.o -lstdc++
cp "$ROOT/python/metakit.py" "$ROOT"/python/test/*.py run/
cd run

failed=0
for t in test_sizeerror test_inttypes test_stringtype test_unittest; do
  echo "== $t"
  if $PYTHON -c "import $t"; then
    echo "   passed"
  else
    echo "   FAILED"
    failed=1
  fi
done
# and the way the project runs them (the same tests, through test.regrtest)
echo "== all.py"
if $PYTHON all.py; then
  echo "   passed"
else
  echo "   FAILED"
  failed=1
fi

exit $failed
