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
