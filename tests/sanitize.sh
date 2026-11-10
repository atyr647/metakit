#!/bin/sh
# Build the library and the tests with AddressSanitizer and
# UndefinedBehaviorSanitizer and run them: regress, twide, tcrash, tlock and,
# when given as the last argument, the randomized tests (tfuzz).
#
# Usage: tests/sanitize.sh [workdir] [tfuzz]
#
# Memory errors and leaks fail the run, and so does any report of undefined
# behaviour.

set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(cd "${1:-.}" && pwd)/sanitize-work
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

SAN="-fsanitize=address,undefined"
CXXFLAGS="-g -O1 -fno-omit-frame-pointer $SAN" LDFLAGS="$SAN" \
  "$ROOT/unix/configure" > /dev/null 2>&1
TARGETS="libmk4.so regress twide tcrash tlock"
[ "$2" = tfuzz ] && TARGETS="$TARGETS tfuzz"
make -j4 $TARGETS > build.log 2>&1 || { tail build.log; exit 1; }

export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=0
export LD_LIBRARY_PATH=.
mkdir -p tests
cp "$ROOT/unix/reversed" .

run() {
  name=$1; shift
  echo "== $name"
  "$@" > "$name.out" 2> "$name.err" || { tail -20 "$name.err"; echo "FAILED"; exit 1; }
  if grep -q "AddressSanitizer\|LeakSanitizer" "$name.err"; then
    grep -A12 "Sanitizer" "$name.err" | head -40
    echo "FAILED: memory error"
    exit 1
  fi
  unknown=$(grep "runtime error" "$name.err" || true)
  if [ -n "$unknown" ]; then
    echo "$unknown" | sort -u | head
    echo "FAILED: undefined behaviour"
    exit 1
  fi
  echo "   clean"
}

run regress ./regress
diff -r "$ROOT/tests/ok" tests > /dev/null || { echo "output differs"; exit 1; }
run twide ./twide
run tcrash ./tcrash
run tlock ./tlock
[ "$2" = tfuzz ] && run tfuzz ./tfuzz
echo "== all clean"
