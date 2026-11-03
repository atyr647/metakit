// tlarge.cpp -- Large datafile test program (not part of the regress suite)
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

// Builds a datafile of several gigabytes by committing one big memo at a
// time, then reopens it and verifies every byte.  It takes a while and needs
// plenty of disk space and RAM, so it is not run by "make test", use
// "make test-large" instead.
//
// Whatever the engine can or cannot do, the following must always hold:
//   - a commit either succeeds or fails, a failed commit must never damage
//     what was committed before it, nor what is committed after it
//   - everything that was committed successfully can be read back unchanged
//
// Usage: tlarge <file> [rows] [megabytes-per-row] [--expect-all]
//   --expect-all  also demand that every commit succeeds (64-bit engine)

#include "mk4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long long i64;

static int sFailures = 0;

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

// compare a stored memo with what Fill() would have produced
static bool VerifyRow(const c4_View &v_, const c4_BytesProp &p_, int row_,
  i64 len_) {
  c4_RowRef r = v_[row_];
  if ((i64)p_(r).GetSize() != len_) {
    fprintf(stderr, "row %d: size %lld, expected %lld\n", row_,
      (i64)p_(r).GetSize(), len_);
    return false;
  }

  const int kChunk = 1 << 20;
  for (i64 off = 0; off < len_;) {
    c4_Bytes data = p_(r).Access((t4_i32)off, kChunk);
    int n = data.Size();
    if (n <= 0) {
      fprintf(stderr, "row %d: no data at %lld\n", row_, off);
      return false;
    }
    const t4_byte *q = data.Contents();
    for (int j = 0; j < n; ++j)
      if (q[j] != PatternByte(row_, off + j)) {
        fprintf(stderr, "row %d: mismatch at offset %lld\n", row_, off + j);
        return false;
      }
    off += n;
  }
  return true;
}

// reopen read-only and check the first "rows" rows, and the optional extra
static void VerifyFile(const char *fn_, int rows_, i64 len_, bool extra_) {
  c4_Storage s(fn_, 0);
  c4_View v = s.View("a");
  c4_BytesProp p("p");
  printf("  reopened: %d rows, file mapped: %s\n", v.GetSize(),
    s.Strategy()._mapStart != 0 ? "yes" : "no");

  CHECK(v.GetSize() == rows_ + (extra_ ? 1 : 0));
  for (int i = 0; i < rows_ && i < v.GetSize(); ++i)
    CHECK(VerifyRow(v, p, i, len_));
  if (extra_ && v.GetSize() == rows_ + 1)
    CHECK(VerifyRow(v, p, rows_, 1000));
}

int main(int argc, char **argv) {
  const char *fn = argc > 1 ? argv[1] : "tlarge.mk";
  int rows = argc > 2 ? atoi(argv[2]) : 6;
  i64 len = (argc > 3 ? atoi(argv[3]) : 512) * (i64)(1 << 20);
  bool expectAll = argc > 4 && strcmp(argv[4], "--expect-all") == 0;

  remove(fn);

  int good = 0, failedAt =  - 1;
  bool smallRow = false;
  t4_byte *buf = (t4_byte*)malloc((size_t)len);
  if (buf == 0) {
    fprintf(stderr, "out of memory\n");
    return 2;
  }

  {
    c4_Storage s(fn, 1);
    c4_View v = s.GetAs("a[p:B]");
    c4_BytesProp p("p");

    for (int i = 0; i < rows; ++i) {
      Fill(buf, i, len);
      v.Add(p[c4_Bytes(buf, (int)len)]);

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

  printf("verify after %d good commits\n", good);
  VerifyFile(fn, good, len, smallRow);

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
    c4_Storage s2(fn, 0);
    c4_View v2 = s2.View("a");
    c4_BytesProp p2("p");
    CHECK(v2.GetSize() == good + 2);
    if (v2.GetSize() == good + 2) {
      CHECK(VerifyRow(v2, p2, good, 1000));
      for (int i = 0; i < good; ++i)
        CHECK(VerifyRow(v2, p2, i, len));
    }
  }

  if (expectAll)
    CHECK(failedAt < 0);

  free(buf);
  printf(sFailures == 0 ? "PASSED\n" : "FAILED (%d checks)\n", sFailures);
  return sFailures == 0 ? 0 : 1;
}
