#!/bin/sh
# Show that this version of Metakit is compatible with an older one, by
# building both and comparing what they make of each other's datafiles.
#
# Usage: tests/compat/compat.sh [old-git-ref] [workdir]
#
# The old reference is the last commit before the 64-bit work by default.
# This needs git, a C++ compiler and a few seconds.  The checks are:
#
#   1. Datafiles below 2 GB are written byte for byte as before
#   2. Each version reads what the other wrote, with the same result
#   3. A datafile with wide marks (beyond 2 GB) is not understood by the old
#      version: it sees no data at all.  What happens to it then, for a
#      program which opens it just to read, or which opens it to change it?

set -e
OLD_REF=${1:-ab38c53}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(cd "${2:-.}" && pwd)/compat-work
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

fail() { echo "FAILED: $*"; exit 1; }

echo "== building the old version ($OLD_REF) and the current one"
git -C "$ROOT" worktree add --detach "$WORK/old-src" "$OLD_REF" >/dev/null 2>&1 ||
  fail "cannot check out $OLD_REF"
for v in old new; do
  if [ $v = old ]; then SRC="$WORK/old-src"; else SRC="$ROOT"; fi
  mkdir -p $v
  (cd $v && "$SRC/unix/configure" >/dev/null 2>&1 &&
    make libmk4.so >/dev/null 2>&1) || fail "cannot build $v"
  g++ -O1 -I"$SRC/include" -o $v/compat "$ROOT/tests/compat/compat.cpp" \
    -L$v -lmk4 -lpthread || fail "cannot build the helper for $v"
done
g++ -O1 -I"$ROOT/include" -o new/compatwide "$ROOT/tests/compat/compatwide.cpp" \
  -Lnew -lmk4 -lpthread
git -C "$ROOT" worktree remove --force "$WORK/old-src"

run() { v=$1; shift; LD_LIBRARY_PATH=$WORK/$v $WORK/$v/compat "$@"; }

echo "== 1. the same datafiles, byte for byte"
mkdir out-old out-new
run old gen out-old
run new gen out-new
for f in m1.mk m2.mk; do
  cmp out-old/$f out-new/$f || fail "$f differs"
  echo "   $f: identical ($(wc -c < out-old/$f) bytes)"
done

echo "== 2. reading each other's datafiles"
for f in m1.mk m2.mk; do
  a=$(run old digest out-old/$f)
  for v in old new; do
    for d in old new; do
      b=$(run $v digest out-$d/$f)
      [ "$a" = "$b" ] || fail "$v reading $d/$f: $b instead of $a"
    done
  done
  echo "   $f: all four combinations read $a"
done

echo "== 3. a datafile with wide marks, as made for files over 2 GB"
LD_LIBRARY_PATH=$WORK/new ./new/compatwide wide.mk
want=$(run new digest wide.mk)
echo "   current version reads: $want"
case "$want" in "rows=0 "*) fail "the current version cannot read it";; esac
cp wide.mk wide.orig
sum0=$(cksum < wide.mk)

echo "   old version, opened to read:   $(run old digest wide.mk)"
[ "$(cksum < wide.mk)" = "$sum0" ] || fail "reading changed the datafile"
echo "   old version, opened to change: $(run old digest wide.mk rw)"
[ "$(cksum < wide.mk)" = "$sum0" ] || fail "opening changed the datafile"
echo "   datafile unchanged by both"

echo "   old version, adding rows to it: $(run old append wide.mk)"
size=$(wc -c < wide.orig)
cmp -n "$size" wide.orig wide.mk || fail "the existing bytes were changed"
echo "   all $size existing bytes are still as they were,"
echo "   the old version has put its own datafile behind them"
echo "   current version now reads: $(run new digest wide.mk)"

echo "== 4. mkinfo tells them apart, without any version of the library"
g++ -O1 -o mkinfo "$ROOT/demos/mkinfo.cpp"
[ "$(./mkinfo -q out-new/m1.mk)" = standard ] || fail "mkinfo standard"
./mkinfo -q out-new/m2.mk > /dev/null || fail "mkinfo exit status for standard"
if ./mkinfo -q wide.orig > mk.out; then fail "mkinfo exit status for wide"; fi
[ "$(cat mk.out)" = wide ] || fail "mkinfo wide: $(cat mk.out)"
st=0; ./mkinfo -q wide.orig > /dev/null || st=$?
[ $st = 3 ] || fail "mkinfo exit status for wide is $st"
head -c 3000000 out-new/m2.mk > cut.mk
[ "$(./mkinfo -q cut.mk)" = damaged ] || fail "mkinfo on a cut file"
echo "   standard, wide (exit 3) and damaged are told apart"

echo "== all as expected"
