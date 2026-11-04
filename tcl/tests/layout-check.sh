#!/bin/sh
# Check that Mk4tcl does not depend on how memory happens to be laid out.
#
# A bug of that kind, in the way names of properties were remembered (see
# AsProperty in mk4tcl.cpp), only showed after an unrelated change to a class
# made the memory be used differently, and no memory checker can find such a
# thing.  This builds Mk4tcl in several ways in which the members of c4_Persist
# have other offsets, runs the tests of each, and requires the same results
# as without any change.  Padding is put in by sed on a copy of the sources.
#
# Usage: tcl/tests/layout-check.sh [workdir]    (needs Tcl 8.6 and g++)

set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(cd "${1:-.}" && pwd)/layout-work
TCLCONF=${TCLCONF:-/usr/lib/tcl8.6}
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

results() { # build directory, name of the variant
  (cd "$1" && "$ROOT/tcl/configure" --with-tcl="$TCLCONF" CC=g++ > /dev/null 2>&1)
  (cd "$1" && make -j4 > build.log 2>&1) || { tail "$1/build.log"; exit 1; }
  (cd "$1" && make test 2>&1 | grep -E '^==== .* FAILED|Total' | sed 's/Total.*Passed/Passed/' | sort)
}

# a copy of the sources, in which c4_Persist has some padding
variant() { # name, sed expression for persist.h, for persist.cpp
  rm -rf "src-$1"
  mkdir "src-$1"
  cp -r "$ROOT/src" "$ROOT/include" "$ROOT/tcl" "src-$1/"
  mkdir -p "src-$1/unix" && cp "$ROOT/unix/reversed" "src-$1/unix/" 2>/dev/null || true
  sed -i "$2" "src-$1/src/persist.h"
  sed -i "$3" "src-$1/src/persist.cpp"
  mkdir "build-$1"
  (cd "build-$1" && "$WORK/src-$1/tcl/configure" --with-tcl="$TCLCONF" CC=g++ > /dev/null 2>&1)
  (cd "build-$1" && make -j4 > build.log 2>&1) || { tail "build-$1/build.log"; exit 1; }
  (cd "build-$1" && make test 2>&1 | grep -E '^==== .* FAILED|Total' | sed 's/Total.*Passed/Passed/' | sort)
}

echo "== unchanged"
mkdir build-plain
plain=$(results build-plain plain)
echo "$plain"

fail=0
check() {
  echo "== $1"
  if [ "$2" = "$plain" ]; then echo "   same results"; else echo "$2"; echo "   DIFFERENT from the unchanged build"; fail=1; fi
}

# a byte in front of the members which come last (this is what showed it)
r=$(variant after-rootwalk 's|^    c4_Bytes _rootWalk;|    c4_Bytes _rootWalk; bool _pad1;|' 's|_differ(0), _fCommit(0)|_differ(0), _pad1(false), _fCommit(0)|')
check "padding after _rootWalk" "$r"
# and 8 bytes, where every member after it moves by 8
r=$(variant after-root 's|^    c4_HandlerSeq \*_root;|    c4_HandlerSeq *_root; void *_pad2;|' 's|_root(0), _differ(0)|_root(0), _pad2(0), _differ(0)|')
check "padding after _root" "$r"
exit $fail
