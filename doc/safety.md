# What a commit writes, in what order, and what a crash leaves

This is how `c4_SaveContext::SaveIt` (`src/persist.cpp`) writes a commit to a
datafile, read in full and checked by logging every `DataWrite` and
`DataCommit` call of a real commit (a `c4_FileStrategy` subclass which prints
them). Byte layout of the marks: `src/marks.h`.

## Words

- Header: 8 bytes at offset 0, `JL 1A 00 <size>`. Extend mode marks use `0A`
  in the third byte.
- Skip tail: `80 00 00 00 <its own position>`, 8 bytes. Commit tail:
  `80 <root length, 24 bits> <root position, 32 bits>`, 8 bytes. A standard
  file ends with the two of them (16 bytes). Wide files (over 2 GB) have 24
  bytes of tails: skip, extension, commit tail.
- `DataCommit(0)` is `fflush` and nothing else: it hands the bytes to the
  operating system. They are not on the disk after it.

## The order, default mode (mode 1), a commit which makes the file longer

The file is `header, data, skip tail, commit tail`, `E` bytes long. The new data
end is `L`, so the new file is `L + 16` bytes.

1. Skip tail written at `L` (the file is now `L + 8` bytes, the gap from `E`
   reads as zeros). `fflush`. This is "write the new end marker before
   anything else" (a 2005 change).
2. An 8-byte extension mark `JL 0A 00 <L + 16 - E>` at `E`: it says how far
   the next commit ends, and `RecoverEnd` follows these marks forward from
   the header.
3. The skip tail again, at `L` (same bytes), then the changed columns, then the
   root description last. All of these go into free space inside the file (free
   in the committed state, so nothing of the committed state is overwritten) or
   behind `E`. `fflush`.
4. **The commit tail**, 8 bytes at `L + 8`. The file is now `L + 16` bytes.
   `fflush`.
5. The header is rewritten with the new size. `fflush`, and the file is mapped
   again.

First commit of a new file: the header goes first, then as above.
Extend mode (mode 2): as above, nothing in the old file is overwritten, and the
header is not rewritten (except in a new file). Wide marks: the commit tail
write is 16 bytes (extension and commit tail), and the file always grows.
In place (mode 1, the new commit fits exactly in the free space of the file, so
the file does not grow): there are no steps 1 to 2; the data goes into free
space, step 4 is an **overwrite** of the old commit tail at `E - 8`, and the
header is rewritten with the same size.

## Which write is the commit point

A reader (`c4_Strategy::ScanMarks`) starts at the end of the file: a commit
tail there gives the root, the skip tail before it leads to the header, and the
header is only checked to be a header. **The commit point is step 4**: the
moment those last 8 bytes are in the file. Whoever reads the file after that
sees the new commit, whatever the header says.

The header is the last write, but it is not what makes a commit visible. It is
the way back when the file does not end with valid tails (`RecoverEnd`): the
size in it is where the last complete commit ended, and the extension marks
lead on from there. If a commit is interrupted after step 4 and before step 5,
the file reads as the new commit and the header is one commit behind; the next
commit repairs it before writing anything (`SaveIt`, "make the header right").

So the two descriptions (survey 1: the tail is the commit point; MK Studio
`store.tcl`: the header is written last) are both true of the order, and only
the first is about when a commit becomes visible.

## What a crash leaves, if writes reach the disk in the order they are made

(A killed process: the operating system keeps everything written, so this is
what it looks like. `tests/twide.cpp` cuts a commit after every write.)

| Crash | The file reads as |
|---|---|
| before step 1, or in 1 to 3 | the old commit: it ends with a skip tail or with data, not a commit tail, so `RecoverEnd` follows the header to the old end |
| after step 4 | the new commit |
| after step 4, before 5 | the new commit, header behind |
| in place: before step 4 | the old commit |
| in place: after step 4 | the new commit |

## What a power cut changes

`fflush` is not a barrier. The disk may write the last 8 bytes (step 4)
before the data of step 3, or the data only in part, and a power cut then
leaves a valid commit tail which leads to blocks that never arrived. This was
not safe, and there was no `fsync` anywhere in the commit path.

Second, the in-place overwrite (mode 1 files which do not grow, which is the
steady state of a database that is only updated) replaces 8 bytes in the middle
of the file. A disk only writes a single sector (512 bytes) as one unit. If
those 8 bytes straddle a sector boundary (7 positions in 512), a power cut can
leave half of the old tail and half of the new one.

## Durability: what a commit does now

A commit forces its data to disk twice (`c4_Strategy::DataSync`, which for a
file is `fsync` on Linux and the other Unixes, `fcntl(F_FULLFSYNC)` on macOS
with `fsync` where that is not supported, `FlushFileBuffers` on Windows):

1. after the data, the structure and the root, **before** the commit tail
   is written (step 3 above);
2. after the commit tail is written (step 4), so that when `Commit` returns, the
   commit is on the disk.

The header write (step 5) and the end marker (step 1) are not forced: neither
decides what a reader finds (see above), and the model below shows a commit does
not need it. The bytes written are exactly the same as before, in the same
order, so the file format is unchanged and older programs read new files (this
is tested, see `tests/compat/compat.sh`).

A new file also has its directory forced to disk when it is created (Unix), or
a power cut can lose the file as a whole.

If forcing fails, `Commit` returns false. Before the commit tail: nothing was
made visible. After it: the commit is in the file, but it is not certain that it
is on the disk.

**Cost.** Two forced writes per commit. On the machine this was made on (ext4
on a virtual disk) a commit of a small change took 0.014 ms before and 7.9 ms
after, and a file system with a slow `fsync`, or macOS with `F_FULLFSYNC`,
is slower than that. A program which commits in a loop to load data should
commit less often, or set the environment variable `METAKIT_NOSYNC` (to
anything but empty or 0) to turn the forcing off: that is the old behaviour.

**Commits in place.** A commit in default mode which fits in the free space of
the file overwrote the old 8-byte commit tail in the middle of the file. If
those 8 bytes straddle a boundary between two 512-byte sectors, a disk can
write one sector and not the other, and a power cut leaves half an old tail
and half a new one. That is now not done: if the tail would be in two
sectors, the commit extends the file as any other commit does (the next one
is in a different place). This is the only change in what is written, and only
for files in which the tail is in that place; the file format is the same.

**How this was checked** (`tests/tcrash.cpp`, `tests/tsync.sh`, both run by
`make test`):

- `tcrash` is a model of a disk which loses what was not forced: every write is
  pending until `DataSync`, and after each write of a commit, every combination
  of pending writes (a sector at a time) is read back as a datafile. It must be
  the state before the commit or the one after it, not damaged, and must take
  more commits. With `DataSync` before and after the commit tail: 31,794 disks
  tried over four configurations, none wrong. Without any, in default
  mode 42,595 disks were tried and 39,162 were wrong (18,455 found damaged), and
  with it only before the tail, or only after it, some were still wrong (3,240 and
  636 in default mode): both places are needed (`tcrash all` shows the numbers).
  75,470 more with the commit tail at the edge of a sector: none wrong, and 348
  wrong without the change to commits in place (a sweep of 1,100 positions of the
  tail, 8.2 million disks, none wrong).
- `tsync.sh` runs commits under `strace` and checks that the calls are: writes,
  `fsync`, one write of the commit tail, `fsync` (and then the header), in all
  four configurations (modes 1 and 2, standard and wide marks), the directory of
  a new file, and no `fsync` at all with `METAKIT_NOSYNC`.

**What is not shown.** The model is the promise of `DataSync` and the sector
as the unit of writing (some disks promise more, a few less); a real power cut
has not been tried, nor has a disk which lies about forcing data. The macOS and
Windows calls were not run (the Windows branch was not even compiled, there was
no compiler for it): on Linux, `F_FULLFSYNC` was only tried as a made-up
constant to see that the fallback to `fsync` works.

# More than one writer

## What was wrong

Metakit has no locking. Two programs (or two handles in one program) which had a
datafile open for writing each worked on their own picture of it. Each commit
reported success, and the second one wrote over the blocks of the first. Tried
on the version before this one (Tcl, two handles, 10 rows, each adds 4 and
commits): both commits said yes, the file had 14 rows, and the four new ones
read back as 5, 0, 6 and 0: neither writer's values.

## What can and cannot be made right

A datafile is one file which is memory-mapped, and a commit replaces the state of
the whole file. **Two writers working at the same time cannot be merged**: there
is nothing to merge them with, a commit is "this is the new state of everything".
What can be made right is that writers take turns (each one starting from what
the one before it committed), and that a writer which was not told to wait is
refused rather than allowed to overwrite. Nothing here merges anything.

## What happens now

1. **The lock.** A datafile which is opened for writing (mode 1, or 2 for
   extend mode) is locked, exclusively, for as long as it is open (`flock` on
   Linux and the other Unixes, `LockFileEx` on Windows). A second writer does not
   open it: `c4_Storage::Strategy().IsValid()` is false and `Strategy()._failure`
   is `c4_Strategy::kFailLocked`; in Tcl `mk::file open` fails with error code
   `MK LOCKED` and a message which says so. This is also so for a second writer in
   the same program, which was the case which silently lost data.
   Readers (mode 0) do not ask for a lock and do not mind one. The lock is
   advisory: a program which does not ask for it (an older Metakit) is not kept
   out, and does not keep anyone out. The system lets go of the lock when the
   program ends, however it ends (the test kills a writer with `kill -9`): there
   is no lock file to go stale. A child made with `fork` and not `exec` shares the
   open file and with it the lock, so the lock stays as long as the child does.
2. **The escape hatch.** `c4_Storage(file, mode, false)` in C++, `-nolock` in
   `mk::file open`, and the environment variable `METAKIT_NOLOCK` (set to
   anything but empty or 0, which works for every program, Python included) open
   for writing without the lock, as before.
3. **A commit which would replace another is refused.** The size of the file is
   noted when it is read or written, together with its first 8 bytes (the
   header) and its last 24 (the tails of the last commit). A commit which has
   something to write first looks at these again. If they are not the same,
   somebody else has committed in the meantime, and nothing is written:
   `Commit` returns false, `_failure` is `kFailChanged`, and in Tcl the error code is
   `MK CHANGED`. Nothing of the other program's work is lost, and nothing of
   this one's is written; `Rollback` loads what is there, the changes can be made
   again. A commit with nothing to write is not refused.
4. **mk::transaction** (Tcl, see the README) does the whole cycle: take the lock
   (by opening for writing, so it also reads what has been committed by then),
   run a script, commit, let go; and it waits and tries again if the file is
   locked.

## What it does not do

- Writers do not merge, and one which is not waiting is refused: the program has
  to try again.
- The check in 3 can miss a change: two commits in place by someone else, which
  leave the size and the ends of the file as they were, are not seen. It is a
  safety net for programs which left the lock out or run on a file system without
  locks, not a replacement for the lock. (In one try, with a second handle which
  made two such commits, the commit of the first one was not refused and nothing
  showed as lost, because Metakit reads the parts of the file which it has not
  changed straight from the file; I did not find out how far that goes.)
- `flock` on a network file system may not work, or may work only between
  programs on the same machine; where it cannot lock at all, nothing is locked
  (only a lock held by somebody else is an error). Not tried here.
- The lock on Windows has not been run, nor compiled (no compiler for it here).

## How this was checked

- `tests/tlock.cpp` (run by `make test`): a second writer is refused, also in the
  same program and with a new file; readers are not; the lock goes with the
  close; the three ways to leave it out; a commit which would replace another is
  refused, in both modes, for added rows, for changes in place, for a new file,
  and refused again; a writer which has nothing to commit is not; a writer which
  is killed with `kill -9` leaves nothing behind; three programs which take
  turns, 20 commits each, lose none of their 60 rows; two programs without the
  lock, one is refused and the other's rows are there. Without the lock code, or
  without the check in 3, it fails.
- `tcl/tests/lock.test`: the same through Tcl, and `mk::transaction`: results,
  failures which commit nothing, waiting for a program which holds the file,
  giving up with a code, and three Tcl programs which take turns.
