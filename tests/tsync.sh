#!/bin/sh
# Check, in the system calls, that a commit forces its data to disk twice: once
# before the commit tail is written (everything which the tail leads to), and
# once after it.  The power cut model (tcrash) assumes that this is what
# c4_FileStrategy::DataSync does, and this is the check that it does.
#
# Usage: tests/tsync.sh [builddir]    (where tsync was built; needs strace)
#
# A commit must make these calls on the datafile, in this order:
#   writes, fsync, ONE write of 8 bytes (16 for wide marks: the commit tail),
#   fsync, and then at most one write of 8 bytes (the header).
# A new file must have its directory forced to disk too, and with
# METAKIT_NOSYNC set there must be no fsync at all.

BUILD=$(cd "${1:-.}" && pwd)
cd "$BUILD" || exit 1

if ! command -v strace > /dev/null 2>&1; then
  echo "tsync.sh: strace not found, skipped"
  exit 0
fi
[ -x ./tsync ] || { echo "tsync.sh: no tsync program in $BUILD"; exit 1; }

export LD_LIBRARY_PATH=.
fail() { echo "FAILED: $*"; exit 1; }

# the calls of a run, one line per call: marker, write <length>, fsync <fd>
calls() {
  awk '
    /openat\(.*"tsync\.mk".*= [0-9]+$/ { fd = $NF; next }
    /openat\(.*"\.", O_RDONLY.*= [0-9]+$/ { dirs[$NF] = 1; next }
    /write\(2, "COMMIT/ { print "commit"; next }
    /^[0-9]+ +write\(/ {
      if ($0 ~ "write\\(" fd ",") { n = $NF; print "write", n }
      next }
    /^[0-9]+ +fsync\(/ {
      match($0, /fsync\([0-9]+/); f = substr($0, RSTART + 6, RLENGTH - 6)
      if (f == fd) print "fsync"
      else if (f in dirs) print "dirsync"
      next }
  ' "$1"
}

check() {  # $1 = mode, $2 = "" or wide, $3 = length of the commit tail write
  rm -f tsync.mk strace.out
  strace -f -o strace.out -e trace=openat,write,fsync,fdatasync,fcntl \
    ./tsync tsync.mk "$1" 4 $2 2> /dev/null || fail "tsync mode $1 $2"
  calls strace.out > calls.out
  grep -q '^dirsync' calls.out || fail "mode $1 $2: new file, directory not forced"

  # each commit: writes, force, one write (the tail), force, at most one write
  # of 8 bytes (the header)
  awk -v tail="$3" '
    function done(   ok) {
      if (!started) return
      ok = (syncs == 2 && before >= 1 && between == 1 && betweenLen == tail && after <= 1 && (after == 0 || afterLen == 8))
      commits++
      if (!ok) { bad = 1; print "commit " commits ": writes before " before ", forces " syncs ", between " between " (" betweenLen " bytes), after " after }
    }
    $1 == "commit" { done(); started = 1; syncs = before = between = after = 0; next }
    started && $1 == "write" {
      if (syncs == 0) before++
      else if (syncs == 1) { between++; betweenLen = $2 }
      else { after++; afterLen = $2 }
      next }
    started && $1 == "fsync" { syncs++; next }
    END { done(); if (commits != 4) { bad = 1; print commits " commits, not 4" } exit bad }
  ' calls.out || fail "mode $1 $2: the calls of a commit are not as they should be"
  echo "  mode $1 $2: 4 commits, each: writes, force, tail of $3 bytes, force"
}

echo "== system calls of commits"
check 1 "" 8
check 2 "" 8
check 1 wide 16
check 2 wide 16

echo "== METAKIT_NOSYNC"
rm -f tsync.mk strace.out
METAKIT_NOSYNC=1 strace -f -o strace.out -e trace=openat,write,fsync,fdatasync,fcntl \
  ./tsync tsync.mk 1 3 2> /dev/null || fail "tsync with METAKIT_NOSYNC"
calls strace.out | grep -q 'sync' && fail "METAKIT_NOSYNC did not turn forcing off"
echo "  no fsync"
rm -f tsync.mk strace.out calls.out
echo "== all as expected"
