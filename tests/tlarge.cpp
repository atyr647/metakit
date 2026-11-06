// tlarge.cpp -- Large datafile test program (not part of the regress suite)
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

// Builds a datafile of several gigabytes by committing one big memo at a
// time, and verifies what is read back.  It takes a while and needs plenty
// of disk space and RAM, so it is not run by "make test", use
// "make test-large" instead.  The steps are:
//
//   1. Commit rows until the file is well beyond 2 Gb (and 4 Gb), then
//      reopen it, check it is memory-mapped, and compare every byte.
//   2. Commit a small change to this large file, in the same session and in
//      a new one, and check the data again.
//   3. Remove rows at the start, which frees space below 2 Gb, and add a
//      new row: this must go into the freed space instead of making the
//      file larger (so the allocator works across the boundary).
//   4. Interrupt commits of a large row at several points, check that the
//      last complete commit is intact each time, and that more commits can
//      follow on such a file.
//   5. Commit a column of integers which is far beyond 256 Mb (it used to
//      overflow an int calculation there), which is as large as one column
//      is allowed to get.  Persisted values are read back and checked.
//   6. Memos and a column of integers at the limit for a column (2^31 - 1
//      bytes) and around 2^31 - 4096: positions in a column include its gap,
//      which made them go beyond 31 bits there, and the program crashed.
//      They are made in one piece and in small pieces, committed, reopened,
//      changed at the limit, and what goes beyond it has to be refused.
//      This needs a 64-bit process, some 8 Gb of memory (of which the mapped
//      datafile is a part) and 4.3 Gb of disk.
//
// Whatever the engine can or cannot do, the following must always hold:
//   - a commit either succeeds or fails, a failed commit must never damage
//     what was committed before it, nor what is committed after it
//   - everything that was committed successfully can be read back unchanged
//
// Usage: tlarge <file> [rows] [megabytes-per-row] [flags]
//   --expect-all  also demand that every commit succeeds (needs 64-bit
//                 file positions) and runs the steps after the first one
//   --unmapped    expect that the library is built without file mapping
//   --ints=<n>    use n million integers in step 5 (default 150), which a
//                 32-bit process needs to keep below some 100
//   --limit-only  only run step 6 (the rows and megabytes are not used)

#include "mk4.h"
#include "mk4io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long long i64;

static int sFailures = 0;
static bool sExpectMapped = true;
static int sMillionInts = 150;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "CHECK FAILED line %d: %s\n", __LINE__, #c); \
    ++sFailures; } } while (0)

static t4_byte PatternByte(int row_, i64 k_) {
  return (t4_byte)(k_ * 31 + (k_ >> 12) * 7 + (k_ >> 28) * 13 + row_ * 101);
}

static void Fill(t4_byte *buf_, int row_, i64 len_) {
  for (i64 k = 0; k < len_; ++k)
    buf_[k] = PatternByte(row_, k);
}

// What should be in the file: a number identifying the contents of the memo
// (it determines every byte of it) and its length.
struct Row {
    int _id;
    i64 _len;
};

// compare a stored memo with what Fill() would have produced, and unless
// "full" is set, only compare the ends and a sample of the rest
static bool VerifyRow(const c4_View &v_, const c4_BytesProp &p_, int index_,
  const Row &row_, bool full_) {
  c4_RowRef r = v_[index_];
  if ((i64)p_(r).GetSize() != row_._len) {
    fprintf(stderr, "row %d: size %lld, expected %lld\n", index_,
      (i64)p_(r).GetSize(), row_._len);
    return false;
  }

  const int kChunk = 1 << 20;
  for (i64 off = 0; off < row_._len;) {
    c4_Bytes data = p_(r).Access((t4_i32)off, kChunk);
    int n = data.Size();
    if (n <= 0) {
      fprintf(stderr, "row %d: no data at %lld\n", index_, off);
      return false;
    }

    const t4_byte *q = data.Contents();
    const bool edge = off < kChunk || off + kChunk >= row_._len;
    const int step = full_ || edge ? 1: 4093;
    for (int j = 0; j < n; j += step)
      if (q[j] != PatternByte(row_._id, off + j)) {
        fprintf(stderr, "row %d: mismatch at offset %lld\n", index_, off + j);
        return false;
      }

    off += n;
    if (!full_ && !edge)
      off += 31 * (i64)kChunk; // skip ahead, then sample again
    if (off > row_._len - kChunk && off < row_._len)
      off = row_._len - kChunk;
  }
  return true;
}

// reopen read-only and check that the rows are all there
static void VerifyFile(const char *fn_, const Row *rows_, int n_, bool full_) {
  // A column which has been accessed stays in memory until the storage is
  // closed, if the file cannot be mapped.  A process which cannot hold all
  // the rows at once then looks at one row in each session.
  if (sizeof(void*) < 8) {
    for (int i = 0; i < n_; ++i) {
      c4_Storage s(fn_, 0);
      c4_View v = s.View("a");
      c4_BytesProp p("p");
      CHECK(v.GetSize() == n_);
      if (v.GetSize() == n_)
        CHECK(VerifyRow(v, p, i, rows_[i], full_));
    }
    c4_Storage s(fn_, 0);
    printf("  reopened row by row: %d rows, %lld bytes\n", n_,
      (i64)s.Strategy().FileSize());
    return ;
  }

  c4_Storage s(fn_, 0);
  c4_View v = s.View("a");
  c4_BytesProp p("p");
  const bool mapped = s.Strategy()._mapStart != 0;
  printf("  reopened: %d rows, %lld bytes, file mapped: %s\n", v.GetSize(),
    (i64)s.Strategy().FileSize(), mapped ? "yes" : "no");

  // a 64-bit process can map all of it, a 32-bit process cannot
  if (sizeof(void*) >= 8)
    CHECK(mapped == sExpectMapped);

  CHECK(v.GetSize() == n_);
  for (int i = 0; i < n_ && i < v.GetSize(); ++i)
    CHECK(VerifyRow(v, p, i, rows_[i], full_));
}

// a strategy which stops writing after a number of calls, as in a crash
class CrashStrategy: public c4_FileStrategy {
  public:
    int _budget;

    CrashStrategy(int budget_): _budget(budget_){}

    virtual void DataWrite(t4_fpos pos_, const void *buf_, int len_) {
      if (_budget-- > 0)
        c4_FileStrategy::DataWrite(pos_, buf_, len_);
    }

    virtual void DataCommit(t4_fpos newSize_) {
      if (_budget > 0)
        c4_FileStrategy::DataCommit(newSize_);
    }
};

static Row sRows[64];
static int sCount = 0;

static void AddRow(c4_View &v_, const c4_BytesProp &p_, t4_byte *buf_, int id_,
  i64 len_) {
  Fill(buf_, id_, len_);
  v_.Add(p_[c4_Bytes(buf_, (int)len_)]);
  sRows[sCount]._id = id_;
  sRows[sCount]._len = len_;
  ++sCount;
}

// The limit for a column, and the sizes around 2^31 - 4096 at which a memo
// used to crash the program (2^31 - 4096 itself did not, one more did).
static const i64 kLimit = 0x7fffffff;
static const i64 kSizes[] =  {
  kLimit - 4096, kLimit - 4095, kLimit - 4094, kLimit
};

// a memo made by writing it in pieces, as a Tcl channel does (4 Kb)
static bool AppendMemo(c4_View &v_, const c4_BytesProp &p_, const t4_byte
  *data_, i64 len_, int piece_) {
  for (i64 off = 0; off < len_; off += piece_) {
    int n = len_ - off < piece_ ? (int)(len_ - off): piece_;
    if (!p_(v_[0]).Modify(c4_Bytes(data_ + off, n, false), (t4_i32)off))
      return false;
  }
  return true;
}

// what goes beyond the limit is refused, and the memo stays as it is
static void CheckRefused(c4_View &v_, const c4_BytesProp &p_, const Row &row_)
  {
  const t4_byte one = 'x';
  const i64 room = kLimit - row_._len;
  static t4_byte more[8192];
  CHECK(!p_(v_[0]).Modify(c4_Bytes(more, (int)room + 1), (t4_i32)row_._len));
  CHECK(!p_(v_[0]).Modify(c4_Bytes(more, (int)room + 1), 12345, (int)room + 1));
  if (room == 0) {
    CHECK(!p_(v_[0]).Modify(c4_Bytes(&one, 1), (t4_i32)row_._len));
    CHECK(!p_(v_[0]).Modify(c4_Bytes(&one, 1), 0, 1));
  }
  CHECK((i64)p_(v_[0]).GetSize() == row_._len);
  CHECK(VerifyRow(v_, p_, 0, row_, false));
}

static void TestColumnLimit(const char *fn_) {
  printf("6. memos and a column of integers at the limit for a column\n");
  if (sizeof(void*) < 8) {
    printf("  (skipped, this needs a 64-bit process)\n");
    return ;
  }

  t4_byte *data = (t4_byte*)malloc((size_t)kLimit);
  CHECK(data != 0);
  if (data == 0)
    return ;
  Fill(data, 7, kLimit); // every memo here is a part of this
  c4_BytesProp p("p");

  for (int k = 0; k < (int)(sizeof kSizes / sizeof *kSizes); ++k) {
    Row row;
    row._id = 7;
    row._len = kSizes[k];
    printf("  memo of %lld bytes (2^31 - %lld)\n", row._len, kLimit + 1 -
      row._len);

    // in pieces, the way a channel writes, in a storage without a file
    {
      c4_Storage s;
      c4_View v = s.GetAs("a[p:B]");
      v.SetSize(1);
      CHECK(AppendMemo(v, p, data, row._len, 4096));
      CHECK((i64)p(v[0]).GetSize() == row._len);
      CHECK(VerifyRow(v, p, 0, row, false));
      CheckRefused(v, p, row);
    }

    // in one piece, committed and read back
    remove(fn_);
    {
      c4_Storage s(fn_, 1);
      c4_View v = s.GetAs("a[p:B]");
      v.SetSize(1);
      p(v[0]).SetData(c4_Bytes(data, (int)row._len, false));
      CHECK((i64)p(v[0]).GetSize() == row._len);
      CheckRefused(v, p, row);
      CHECK(s.Commit());
    }
    VerifyFile(fn_, &row, 1, false);

    // changed where it is mapped (out, and back in, the middle), filled up to
    // the limit, and what is too much is refused, then committed again
    {
      c4_Storage s(fn_, 1);
      c4_View v = s.View("a");
      const int hole = 10000, at = 5000;
      CHECK(p(v[0]).Modify(c4_Bytes(), at, - hole));
      CHECK((i64)p(v[0]).GetSize() == row._len - hole);
      CHECK(p(v[0]).Modify(c4_Bytes(data + at, hole, false), at, hole));
      CHECK(VerifyRow(v, p, 0, row, false));

      // (the end of the data is in the gap now, which has to stay below it)
      CHECK(p(v[0]).Modify(c4_Bytes(), (t4_i32)(row._len - 3000), - 2000));
      CHECK(p(v[0]).Modify(c4_Bytes(data + row._len - 3000, 2000, false),
        (t4_i32)(row._len - 3000), 2000));
      CHECK(VerifyRow(v, p, 0, row, false));

      const i64 room = kLimit - row._len;
      if (room > 0) {
        CHECK(p(v[0]).Modify(c4_Bytes(data + row._len, (int)room, false),
          (t4_i32)row._len));
        row._len = kLimit;
      }
      CheckRefused(v, p, row);
      CHECK(s.Commit());
    }
    VerifyFile(fn_, &row, 1, false);
    remove(fn_);
  }
  free(data);

#if q4_EXCEPTIONS
  // a column of 2^28 - 1 longs is 8 bytes less than the limit, and one more
  // is a size error, from which a rollback recovers
  {
    printf("  2^28 - 1 longs, and one more\n");
    const int n = (1 << 28) - 1;
    c4_LongProp pl("l");
    {
      c4_Storage s(fn_, 1);
      c4_View v = s.GetAs("b[l:L]");
      v.SetSize(n);
      for (int i = 0; i < n; ++i)
        pl(v[i]) = ((t4_i64)i << 33) + 5;
      CHECK(s.Commit());

      bool caught = false;
      try {
        v.Add(pl[1]);
      } catch (const c4_SizeError &e) {
        caught = true;
        CHECK(e._wanted == kLimit + 1 && e._limit == kLimit);
      }
      CHECK(caught);
      CHECK(v.GetSize() == n);
      CHECK(!s.Commit());
      CHECK(s.Rollback());

      // and then it can be changed and committed as before
      v = s.View("b");
      CHECK(v.GetSize() == n);
      v.RemoveAt(0);
      v.Add(pl[(t4_i64)n << 33]);
      CHECK(s.Commit());
    }
    c4_Storage s(fn_, 0);
    c4_View v = s.View("b");
    CHECK(v.GetSize() == n);
    int bad = 0;
    for (int i = 0; i < n - 1 && i < v.GetSize(); i += (i < 100000 ? 1 : 997))
      if ((t4_i64)pl(v[i]) != ((t4_i64)(i + 1) << 33) + 5)
        ++bad;
    CHECK(bad == 0 && v.GetSize() == n && (t4_i64)pl(v[n - 1]) == (t4_i64)n <<
      33);
    CHECK((t4_i64)pl(v[n - 2]) == ((t4_i64)(n - 1) << 33) + 5);
    printf("  %d rows, %d mismatches\n", v.GetSize(), bad);
  }
  remove(fn_);
#endif 
}

int main(int argc, char **argv) {
  // (what was done must be seen even if the program is ended abruptly)
  setvbuf(stdout, 0, _IONBF, 0);

  const char *fn = argc > 1 ? argv[1] : "tlarge.mk";
  int rows = argc > 2 ? atoi(argv[2]) : 6;
  i64 len = (argc > 3 ? atoi(argv[3]) : 512) * (i64)(1 << 20);
  bool expectAll = false, limitOnly = false;
  for (int a = 4; a < argc; ++a) {
    if (strcmp(argv[a], "--expect-all") == 0)
      expectAll = true;
    else if (strcmp(argv[a], "--unmapped") == 0)
      sExpectMapped = false;
    else if (strncmp(argv[a], "--ints=", 7) == 0)
      sMillionInts = atoi(argv[a] + 7);
    else if (strcmp(argv[a], "--limit-only") == 0)
      limitOnly = true;
    else {
      fprintf(stderr, "unknown flag %s\n", argv[a]);
      return 2;
    }
  }

  remove(fn);

  if (limitOnly) {
    TestColumnLimit(fn);
    printf(sFailures == 0 ? "PASSED\n" : "FAILED (%d checks)\n", sFailures);
    return sFailures == 0 ? 0 : 1;
  }

  int good = 0, failedAt =  - 1;
  bool smallRow = false;
  t4_byte *buf = (t4_byte*)malloc((size_t)len);
  if (buf == 0) {
    fprintf(stderr, "out of memory\n");
    return 2;
  }

  printf("1. commit %d rows of %lld bytes\n", rows, len);
  {
    c4_Storage s(fn, 1);
    c4_View v = s.GetAs("a[p:B]");
    c4_BytesProp p("p");

    for (int i = 0; i < rows; ++i) {
      sCount = good;
      AddRow(v, p, buf, i, len);

      bool ok = s.Commit();
      printf("commit %d: %s\n", i, ok ? "ok" : "FAILED");
      fflush(stdout);

      if (ok) {
        ++good;
        continue;
      }

      // The first failure must not poison the next commit: the row is still
      // pending, so a retry must again either fail or truly succeed.
      failedAt = i;
      bool again = s.Commit();
      printf("  retry of commit %d: %s\n", i, again ? "ok" : "FAILED");
      fflush(stdout);
      if (again) {
        ++good;
        failedAt =  - 1;
        continue;
      }

      // Drop the row and commit something small in the same session, this
      // is the case in which committed data used to be lost silently.
      s.Rollback();
      v = s.View("a");
      CHECK(v.GetSize() == good);
      for (int k = 0; k < 1000; ++k)
        buf[k] = PatternByte(good, k);
      v.Add(p[c4_Bytes(buf, 1000)]);
      bool small = s.Commit();
      printf("  small row in same session: %s\n", small ? "ok" : "FAILED");
      CHECK(small);
      smallRow = small;
      break;
    }
  }

  sCount = good;
  if (smallRow) {
    sRows[sCount]._id = good;
    sRows[sCount]._len = 1000;
    ++sCount;
  }

  printf("verify after %d good commits\n", good);
  VerifyFile(fn, sRows, sCount, true);

  if (failedAt >= 0) {
    // and once more from a fresh session, after the failure
    printf("commit a second small row from a new session\n");
    {
      c4_Storage s(fn, 1);
      c4_View v = s.View("a");
      c4_BytesProp p("p");
      for (int k = 0; k < 1000; ++k)
        buf[k] = PatternByte(good + 1, k);
      v.Add(p[c4_Bytes(buf, 1000)]);
      CHECK(s.Commit());
    }
    sRows[sCount]._id = good + 1;
    sRows[sCount]._len = 1000;
    ++sCount;
    VerifyFile(fn, sRows, sCount, true);
  }

  if (expectAll)
    CHECK(failedAt < 0);

  if (!expectAll || failedAt >= 0 || sFailures > 0 || rows < 3) {
    free(buf);
    printf(sFailures == 0 ? "PASSED\n" : "FAILED (%d checks)\n", sFailures);
    return sFailures == 0 ? 0 : 1;
  }

  printf("2. small commits to the large file\n");
  {
    c4_Storage s(fn, 1);
    c4_View v = s.View("a");
    c4_BytesProp p("p");
    AddRow(v, p, buf, 100, 1000);
    CHECK(s.Commit());
    AddRow(v, p, buf, 101, 70000); // lands behind the other columns
    CHECK(s.Commit());
  }
  VerifyFile(fn, sRows, sCount, false);

  printf("3. free the first rows, then add one which has to fit there\n");
  i64 sizeBefore = 0, sizeAfter = 0;
  {
    c4_Storage s(fn, 1);
    c4_View v = s.View("a");
    c4_BytesProp p("p");

    v.RemoveAt(0, 2);
    memmove(sRows, sRows + 2, (sCount - 2) *sizeof(Row));
    sCount -= 2;
    CHECK(s.Commit());
    sizeBefore = s.Strategy().FileSize();

    AddRow(v, p, buf, 102, len);
    CHECK(s.Commit());
    sizeAfter = s.Strategy().FileSize();
  }
  printf("  file grew by %lld bytes\n", sizeAfter - sizeBefore);
  CHECK(sizeAfter - sizeBefore < (1 << 20)); // only for the tails and such
  VerifyFile(fn, sRows, sCount, true);

  printf("4. interrupted commits of a large row\n");
  const int budgets[] =  {
    1, 2, 4, 40, 25000
  };
  for (int b = 0; b < (int)(sizeof budgets / sizeof *budgets); ++b) {
    bool committed;
    {
      CrashStrategy cs(budgets[b]);
      cs.DataOpen(fn, 1);
      c4_Storage s(cs, false, 1);
      c4_View v = s.View("a");
      c4_BytesProp p("p");
      Fill(buf, 200 + b, len / 2);
      v.Add(p[c4_Bytes(buf, (int)(len / 2))]);
      s.Commit(); // runs out of writes somewhere, usually

      // (the commit always says it succeeded, as the writes which do not
      // happen are not reported: it was not interrupted if there was budget
      // left, as it is then that it was also made durable, see DataCommit)
      committed = cs._budget > 0;
    }
    printf("  crash after %d writes\n", budgets[b]);

    // A commit which has not been interrupted (rows which are smaller need
    // fewer writes than there are in the budget) has succeeded, and then the
    // row is in the file.  If it was, the file must be as it was before.
    if (committed) {
      printf("  (not interrupted: the commit succeeded)\n");
      sRows[sCount]._id = 200 + b;
      sRows[sCount]._len = len / 2;
      ++sCount;
    }
    VerifyFile(fn, sRows, sCount, false);

    // more commits must be possible, on a file with the remains at the end
    {
      c4_Storage s(fn, 1);
      c4_View v = s.View("a");
      c4_BytesProp p("p");
      AddRow(v, p, buf, 300 + b, 5000 + b);
      CHECK(s.Commit());
    }
    VerifyFile(fn, sRows, sCount, false);
  }

  printf("final check of everything\n");
  VerifyFile(fn, sRows, sCount, true);

  printf("5. a column of %d million integers\n", sMillionInts);
  {
    const int n = sMillionInts * 1000000;
    const char *fn2 = "tlarge_ints.mk";
    remove(fn2);
    c4_LongProp pl("l");
    {
      c4_Storage s(fn2, 1);
      c4_View v = s.GetAs("a[l:L]");
      v.SetSize(n);
      for (int i = 0; i < n; ++i)
        pl(v[i]) = ((i64)i << 20) + 7;
      CHECK(s.Commit());
    }
    c4_Storage s(fn2, 0);
    c4_View v = s.View("a");
    CHECK(v.GetSize() == n);
    int bad = 0;
    for (int i = 0; i < v.GetSize() && i < n; i += (i < 100000 ? 1: 991))
      if ((i64)pl(v[i]) != ((i64)i << 20) + 7)
        ++bad;
    CHECK(v.GetSize() == n && bad == 0 && (i64)pl(v[n - 1]) == ((i64)(n - 1) <<
      20) + 7);
    printf("  %d rows, %d mismatches\n", v.GetSize(), bad);
    remove(fn2);
  }

  // (the file of the steps before is not needed any more, this needs room)
  free(buf);
  remove(fn);
  TestColumnLimit(fn);

  printf(sFailures == 0 ? "PASSED\n" : "FAILED (%d checks)\n", sFailures);
  return sFailures == 0 ? 0 : 1;
}
