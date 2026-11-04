// twide.cpp -- Test program for the wide file marks used beyond 2 Gb
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

// Datafiles which need positions of more than 31 bits get "wide" marks, see
// src/marks.h.  Such files are too large for a regular test, so these tests
// use a hook (f4_WideAbove) to switch to the wide marks at any size, and a
// pretend "file" which only has the marks, to try positions over 4 Gb.
//
// This program is built and run by "make test" on Unix, next to regress.

#include "mk4.h"
#include "mk4io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined (unix) || defined (__unix__) || defined (__APPLE__)
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#define HAVE_FORK 1
#endif 

// the hook which sets the size above which wide marks are used
extern t4_fpos f4_WideAbove;
// and the one which sets the size of datafile which commits cannot exceed
extern t4_fpos f4_MaxFileEnd;
// and the one with the size which a single column cannot exceed
extern t4_i32 f4_MaxColumnSize;

static const t4_fpos kStdLimit = 0x7fffffff - 64; // the default
static const t4_fpos kMaxEndDefault = f4_MaxFileEnd; // what the library has
static int sFailures = 0;
static int sChecks = 0;

#define CHECK(c) do { ++sChecks; if (!(c)) { \
    fprintf(stderr, "CHECK FAILED line %d: %s\n", __LINE__, #c); \
    ++sFailures; } } while (0)

/////////////////////////////////////////////////////////////////////////////
// a deterministic workload, and a way to boil down the contents to a number

static void Mutate(c4_View v_, int round_, int kind_) {
  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_BytesProp pb("b");

  if (kind_ == 0) {
    // add rows, some with memos large enough to be stored separately
    for (int i = 0; i < 200; ++i) {
      char tmp[40];
      sprintf(tmp, "s%d-%d", round_, i);
      int n = v_.Add(pi[i + round_ * 1000] + ps[tmp]);
      if (i % 40 == 0) {
        int len = 9000 + i * 50;
        t4_byte *b = (t4_byte*)malloc(len);
        for (int k = 0; k < len; ++k)
          b[k] = (t4_byte)(k * round_ + i);
        pb(v_[n]).SetData(c4_Bytes(b, len));
        free(b);
      }
    }
  } else if (kind_ == 1) {
    // remove half, this leaves room in the file for later commits
    if (v_.GetSize() > 20)
      v_.RemoveAt(3, v_.GetSize() / 2);
  } else {
    // a tiny change
    if (v_.GetSize() > 5)
      pi(v_[2]) = round_ * 77;
  }
}

// reduce the contents of the "a" view to a number, -1 if it cannot be read
static long Signature(const char *fn_, int *rows_ = 0) {
  c4_Storage s(fn_, 0);
  if (!s.Strategy().IsValid())
    return  - 1;

  c4_View v = s.View("a");
  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_BytesProp pb("b");

  if (rows_ != 0)
    *rows_ = v.GetSize();

  unsigned long h = v.GetSize();
  for (int i = 0; i < v.GetSize(); ++i) {
    h = h * 31 + pi(v[i]);
    for (const char *q = ps(v[i]); *q; ++q)
      h = h * 31 + *q;
    c4_Bytes b;
    pb(v[i]).GetData(b);
    for (int k = 0; k < b.Size(); k += 97)
      h = h * 31 + b.Contents()[k];
    h += b.Size();
  }
  return h;
}

static long LiveSignature(c4_View v_) {
  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_BytesProp pb("b");

  unsigned long h = v_.GetSize();
  for (int i = 0; i < v_.GetSize(); ++i) {
    h = h * 31 + pi(v_[i]);
    for (const char *q = ps(v_[i]); *q; ++q)
      h = h * 31 + *q;
    c4_Bytes b;
    pb(v_[i]).GetData(b);
    for (int k = 0; k < b.Size(); k += 97)
      h = h * 31 + b.Contents()[k];
    h += b.Size();
  }
  return h;
}

static bool ReadFile(const char *fn_, c4_Bytes &data_) {
  FILE *f = fopen(fn_, "rb");
  if (f == 0)
    return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  t4_byte *p = data_.SetBuffer(n);
  bool ok = (long)fread(p, 1, n, f) == n;
  fclose(f);
  return ok;
}

static t4_fpos Get32(const t4_byte *p_) {
  return ((t4_fpos)p_[0] << 24) + (p_[1] << 16) + (p_[2] << 8) + p_[3];
}

static bool SameBytes(const c4_Bytes &a_, const c4_Bytes &b_) {
  return a_.Size() == b_.Size() && memcmp(a_.Contents(), b_.Contents(),
    a_.Size()) == 0;
}

/////////////////////////////////////////////////////////////////////////////
// 0. A commit which fails must leave no trace, in the datafile or in the
//    state of the storage: later commits must come out exactly as if the
//    failed one had never been tried.

static void SmallChange(c4_View v_) {
  c4_IntProp pi("i");
  c4_StringProp ps("s");
  if (v_.GetSize() > 3) {
    pi(v_[2]) = 5555;
    ps(v_[3]) = "small change";
  }
}

static void TestFailedCommit(int mode_, bool wide_) {
  printf("failed commit, mode %d, %s marks\n", mode_, wide_ ? "wide":
    "standard");

  const char *fnFail = "twide_fail.mk";
  const char *fnCtrl = "twide_ctrl.mk";
  remove(fnFail);
  remove(fnCtrl);
  f4_WideAbove = wide_ ? 0 : kStdLimit;

  // the control: the same commits without the one which fails
  {
    c4_Storage s(fnCtrl, mode_);
    c4_View v = s.GetAs("a[i:I,s:S,b:B]");
    Mutate(v, 1, 0);
    CHECK(s.Commit());
    SmallChange(v);
    CHECK(s.Commit());
    Mutate(v, 2, 0);
    CHECK(s.Commit());
    SmallChange(v);
    Mutate(v, 3, 2);
    CHECK(s.Commit());
  }
  c4_Bytes ctrl;
  CHECK(ReadFile(fnCtrl, ctrl));

  {
    c4_Storage s(fnFail, mode_);
    c4_View v = s.GetAs("a[i:I,s:S,b:B]");
    Mutate(v, 1, 0);
    CHECK(s.Commit());
    const long sig1 = LiveSignature(v);

    c4_Bytes before;
    CHECK(ReadFile(fnFail, before));

    // From here on, the datafile may not grow much, which a lot of new data
    // will make it do.  The commit must fail, whichever way it is retried.
    f4_MaxFileEnd = before.Size() + 20000;

    Mutate(v, 2, 0);
    const long sigBig = LiveSignature(v);
    for (int attempt = 0; attempt < 3; ++attempt) {
      CHECK(!s.Commit());

      // nothing was written, and nothing was lost from memory either
      c4_Bytes now;
      CHECK(ReadFile(fnFail, now));
      CHECK(SameBytes(before, now));
      CHECK(LiveSignature(v) == sigBig);
      CHECK(Signature(fnFail) == sig1);
    }

    // rolling back the failed changes gives the last committed state
    CHECK(s.Rollback());
    v = s.View("a");
    CHECK(LiveSignature(v) == sig1);

    // and committing something smaller works, the free space must be right
    SmallChange(v);
    CHECK(s.Commit());
    CHECK(Signature(fnFail) == LiveSignature(v));

    f4_MaxFileEnd = kMaxEndDefault;

    // now the same commits as in the control, which must give the same file
    Mutate(v, 2, 0);
    CHECK(s.Commit());
    CHECK(Signature(fnFail) == LiveSignature(v));
    SmallChange(v);
    Mutate(v, 3, 2);
    CHECK(s.Commit());
    CHECK(Signature(fnFail) == LiveSignature(v));
  }
  c4_Bytes failed;
  CHECK(ReadFile(fnFail, failed));
  CHECK(SameBytes(ctrl, failed));

  // the same for the first commit, which leaves an empty file behind
  remove(fnFail);
  {
    c4_Storage s(fnFail, mode_);
    c4_View v = s.GetAs("a[i:I,s:S,b:B]");
    Mutate(v, 1, 0);
    f4_MaxFileEnd = 5000;
    CHECK(!s.Commit());
    CHECK(Signature(fnFail) == -1 || Signature(fnFail) == 0);

    f4_MaxFileEnd = kMaxEndDefault;
    CHECK(s.Commit());
    CHECK(Signature(fnFail) == LiveSignature(v));
  }

  f4_MaxFileEnd = kMaxEndDefault;
  f4_WideAbove = kStdLimit;
  remove(fnFail);
  remove(fnCtrl);
}

// Without a rollback in between, as it happened in the original bug: the
// changes stay in memory after a failed commit, and the next commit is of a
// different set of changes.  Whatever was allocated for the failed commit must
// be given back, or else the next commit puts its data elsewhere.
static void TestFailedCommitNoRollback(int mode_, bool wide_) {
  printf("failed commit without rollback, mode %d, %s marks\n", mode_, wide_ ?
    "wide": "standard");

  const char *fnFail = "twide_fail.mk";
  const char *fnCtrl = "twide_ctrl.mk";
  remove(fnFail);
  remove(fnCtrl);
  f4_WideAbove = wide_ ? 0 : kStdLimit;

  c4_Bytes ctrl;
  {
    c4_Storage s(fnCtrl, mode_);
    c4_View v = s.GetAs("a[i:I,s:S,b:B]");
    Mutate(v, 1, 0);
    CHECK(s.Commit());
    const int rows = v.GetSize();

    Mutate(v, 2, 0); // never committed, as it makes no sense to do so
    v.SetSize(rows); // so back to what it was, but with changes pending
    SmallChange(v);
    CHECK(s.Commit());
    CHECK(Signature(fnCtrl) == LiveSignature(v));
  }
  CHECK(ReadFile(fnCtrl, ctrl));

  c4_Bytes failed;
  {
    c4_Storage s(fnFail, mode_);
    c4_View v = s.GetAs("a[i:I,s:S,b:B]");
    Mutate(v, 1, 0);
    CHECK(s.Commit());
    const int rows = v.GetSize();

    c4_Bytes before;
    CHECK(ReadFile(fnFail, before));
    f4_MaxFileEnd = before.Size() + 20000;

    Mutate(v, 2, 0);
    CHECK(!s.Commit()); // fails, which must not have any effect
    CHECK(!s.Commit());
    f4_MaxFileEnd = kMaxEndDefault;

    v.SetSize(rows);
    SmallChange(v);
    CHECK(s.Commit());
    CHECK(Signature(fnFail) == LiveSignature(v));
  }
  CHECK(ReadFile(fnFail, failed));
  CHECK(SameBytes(ctrl, failed));

  f4_WideAbove = kStdLimit;
  remove(fnFail);
  remove(fnCtrl);
}

/////////////////////////////////////////////////////////////////////////////
// 0a. A datafile which has been cut short, as by a crash which loses the
//     last part of a file, or a copy which was interrupted

static void WriteFile(const char *fn_, const t4_byte *p_, int n_) {
  FILE *f = fopen(fn_, "wb");
  CHECK(f != 0);
  if (f != 0) {
    CHECK((int)fwrite(p_, 1, n_, f) == n_);
    fclose(f);
  }
}

static void TestTruncation(int mode_, bool wide_) {
  printf("truncated datafile, mode %d, %s marks\n", mode_, wide_ ? "wide":
    "standard");

  const char *fn = "twide_cut.mk";
  const char *fnCut = "twide_cut2.mk";
  remove(fn);
  f4_WideAbove = wide_ ? 0 : kStdLimit;

  // make a datafile with a number of commits, and note what each one gives
  const int kCommits = 5;
  long sigs[kCommits + 1];
  int ends[kCommits + 1];
  sigs[0] = 0; // the empty database, with no rows
  ends[0] = 0;
  {
    c4_Storage s(fn, mode_);
    c4_View v = s.GetAs("a[i:I,s:S,b:B]");
    for (int r = 1; r <= kCommits; ++r) {
      Mutate(v, r, r % 3 == 0 ? 1: r % 4 == 0 ? 2: 0);
      CHECK(s.Commit());
      sigs[r] = LiveSignature(v);
      ends[r] = (int)s.Strategy().FileSize();
      CHECK(Signature(fn) == sigs[r]);
      for (int k = 0; k < r; ++k)
        CHECK(sigs[k] != sigs[r]); // or they cannot be told apart
    }
  }
  f4_WideAbove = kStdLimit;

  c4_Bytes whole;
  CHECK(ReadFile(fn, whole));
  const int size = whole.Size();
  CHECK(size == ends[kCommits]);

  // the lengths to try: the start, around the end of each commit, all over
  const int kMax = 4000;
  int cuts[kMax];
  int n = 0;
  for (int i = 0; i <= 64; ++i)
    cuts[n++] = i;
  for (int r = 1; r <= kCommits; ++r)
    for (int d =  - 40; d <= 40; ++d)
      if (ends[r] + d >= 0 && ends[r] + d <= size)
        cuts[n++] = ends[r] + d;
  for (int L = 0; L <= size; L += 997)
    cuts[n++] = L;
  srand(1234);
  for (int i = 0; i < 300; ++i)
    cuts[n++] = rand() % (size + 1);
  cuts[n++] = size;

  int found[kCommits + 1];
  memset(found, 0, sizeof found);
  int damaged = 0, notBack = 0, stale = 0;

  for (int i = 0; i < n; ++i) {
    const int L = cuts[i];
    WriteFile(fnCut, whole.Contents(), L);

    int rows = 0;
    const long sig = Signature(fnCut, &rows);

    int state =  - 1; // the commit which this is the result of
    for (int r = 0; r <= kCommits; ++r)
      if (sig == sigs[r] && (r > 0 || rows == 0))
        state = r;

    if (state < 0) {
      // Only in mode 1 there is a case in which this can be: the datafile is
      // cut exactly where an older commit ended, which then looks like the
      // last one.  But later commits reuse space inside the file, so that
      // older commit has been partly overwritten.  It is the only commit
      // which a datafile in mode 1 guarantees, extend mode (2) keeps all.
      bool oldEnd = false;
      for (int r = 1; r < kCommits; ++r)
        if (ends[r] == L)
          oldEnd = true;

      if (mode_ == 1 && oldEnd && rows > 0) {
        ++stale;
        continue;
      }

      ++damaged;
      printf("  DAMAGED at length %d of %d: %d rows, signature %ld\n", L, size,
        rows, sig);
      continue;
    }
    ++found[state];

    // what is left of the datafile after an interruption: the commits
    // which are complete in it, and nothing else, may be found
    int last = 0;
    for (int r = 1; r <= kCommits; ++r)
      if (ends[r] <= L)
        last = r;
    CHECK(state <= last);

    if (mode_ == 2) {
      // each commit is added to the end, so up to the last complete one
      // everything is still there, and it is found, whatever follows it
      CHECK(state == last);
    } else if (state != last)
      ++notBack; // the header only tells about the last commit, see below
  }

  CHECK(damaged == 0);
  CHECK(mode_ == 1 || stale == 0);
  CHECK(found[kCommits] > 0);

  printf("  %d lengths tried, found: empty %d, commit 1..%d: ", n, found[0],
    kCommits);
  for (int r = 1; r <= kCommits; ++r)
    printf("%d ", found[r]);
  printf("(%d earlier state, %d stale: only mode 1 can have these)\n", notBack,
    stale);

  // a cut datafile which is opened to be changed must work as well, here
  // the lengths are those where the cut is just before or after a mark
  for (int r = 1; r <= kCommits; ++r)
    for (int d = - 9; d <= 9; d += 3) {
      const int L = ends[r] + d;
      if (L < 0 || L > size)
        continue;

      WriteFile(fnCut, whole.Contents(), L);
      long live = 0;
      {
        c4_Storage s(fnCut, mode_);
        c4_View v = s.GetAs("a[i:I,s:S,b:B]");
        f4_WideAbove = wide_ ? 0 : kStdLimit;
        Mutate(v, 60 + r, 0);
        CHECK(s.Commit());
        f4_WideAbove = kStdLimit;
        live = LiveSignature(v);
      }
      CHECK(Signature(fnCut) == live);
    }

  remove(fn);
  remove(fnCut);
}

/////////////////////////////////////////////////////////////////////////////
// 0b. A single column (or memo) cannot be larger than 2 Gb: this is enforced

#if HAVE_FORK
// run a function in a process of its own, and see if it ended by abort()
static bool Aborts(void (*func_)()) {
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    if (freopen("/dev/null", "w", stderr) == 0) // the message is expected
      _exit(2);
    func_();
    _exit(0); // did not abort
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

static void ManyStrings() {
  f4_MaxColumnSize = 100000;
  c4_Storage s;
  c4_View v = s.GetAs("a[s:S]");
  c4_StringProp ps("s");
  char text[1001];
  memset(text, 'x', 1000);
  text[1000] = 0;
  for (int i = 0; i < 200; ++i)
    v.Add(ps[text]);
}

static void HugeLongs() {
  // 2^28 longs would be a column of 2^31 bytes, which is one too many
  c4_Storage s;
  c4_View v = s.GetAs("a[l:L]");
  c4_LongProp pl("l");
  v.SetSize(268435456);
  pl(v[0]) = (t4_i64)1 << 40;
}

static void OneByteTooMany() {
  f4_MaxColumnSize = 100000;
  c4_Storage s;
  c4_View v = s.GetAs("a[b:B]");
  c4_BytesProp pb("b");
  static t4_byte buf[100001];
  v.SetSize(1);
  pb(v[0]).SetData(c4_Bytes(buf, sizeof buf));
}
#endif 

#if q4_EXCEPTIONS
static int sHandled = 0;

static void ThrowingHandler(const c4_SizeError &e_) {
  ++sHandled;
  throw (int)(e_._limit);
}

// a size error can be recovered from: nothing is done by what causes it, the
// columns of the view still all have the same rows, nothing can be committed
// (since the changes are not complete), and a rollback makes all well again
static void TestSizeErrors() {
  printf("size errors can be recovered from\n");

  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_BytesProp pb("b");
  c4_LongProp pl("l");

  const char *fn = "twide_size.mk";
  for (int mode = 1; mode <= 2; ++mode) {
    remove(fn);
    long committed = 0;
    {
      c4_Storage s(fn, mode);
      c4_View v = s.GetAs("a[i:I,s:S,b:B,l:L]");
      for (int k = 0; k < 20; ++k) {
        int at = v.Add(pi[k]);
        ps(v[at]) = "committed";
        pl(v[at]) = k * 1000003;
      }
      CHECK(s.Commit());
      committed = LiveSignature(v);
    }

    c4_Bytes before;
    CHECK(ReadFile(fn, before));

    f4_MaxColumnSize = 3000;
    {
      c4_Storage s(fn, mode);
      c4_View v = s.GetAs("a[i:I,s:S,b:B,l:L]");
      const int rows = v.GetSize();
      CHECK(rows == 20);

      // text goes into the column of the strings until it is full, and then
      // the row which does not fit is not there, in none of the columns
      bool caught = false;
      int added = 0;
      try {
        for (; added < 1000; ++added) {
          static char text[201];
          memset(text, 'x', 200);
          c4_Row r;
          pi(r) = 7;
          ps(r) = text;
          pl(r) = 9;
          v.Add(r);
        }
      } catch (const c4_SizeError &e) {
        caught = true;
        CHECK(e._limit == 3000);
        CHECK(e._wanted > 3000);
      }
      CHECK(caught);
      CHECK(v.GetSize() == rows + added);
      CHECK(added > 5 && added < 25); // it was a column of 3000 bytes
      for (int k = 0; k < v.GetSize(); ++k) // all there, in every column
        CHECK(pi(v[k]) == (k < rows ? k: 7) && (t4_i64)pl(v[k]) == (k < rows ?
          k * 1000003: 9));

      // nothing can be committed now, and nothing is written
      CHECK(!s.Commit());
      CHECK(!s.Commit());
      c4_Bytes now;
      CHECK(ReadFile(fn, now));
      CHECK(SameBytes(before, now));

      // until what was changed is rolled back
      CHECK(s.Rollback());
      v = s.GetAs("a[i:I,s:S,b:B,l:L]");
      CHECK(v.GetSize() == rows);
      f4_MaxColumnSize = 0x7fffffff;
      CHECK(LiveSignature(v) == committed);

      // and then it works as it did
      int at = v.Add(pi[1000]);
      ps(v[at]) = "after";
      CHECK(s.Commit());
      CHECK(Signature(fn) == LiveSignature(v));
    }

    // setting a value which is too large for a column changes nothing either
    {
      remove(fn);
      c4_Storage s(fn, mode);
      c4_View v = s.GetAs("a[b:B]");
      v.SetSize(1);
      f4_MaxColumnSize = 100;
      static t4_byte big[101];
      bool caught = false;
      try {
        pb(v[0]).SetData(c4_Bytes(big, sizeof big));
      } catch (const c4_SizeError &) {
        caught = true;
      }
      f4_MaxColumnSize = 0x7fffffff;
      CHECK(caught);
      CHECK(pb(v[0]).GetSize() == 0);
      CHECK(!s.Commit());
      CHECK(s.Rollback());
    }

    // two thirds of the longs are enough: this is a column of integers, whose
    // number of rows is part of what must not change (it used to, first)
    {
      remove(fn);
      c4_Storage s(fn, mode);
      c4_View v = s.GetAs("a[l:L]");
      v.SetSize(10);
      for (int k = 0; k < 10; ++k)
        pl(v[k]) = (t4_i64)k << 40;
      f4_MaxColumnSize = 100;
      bool caught = false;
      try {
        v.SetSize(1000);
      } catch (const c4_SizeError &) {
        caught = true;
      }
      f4_MaxColumnSize = 0x7fffffff;
      CHECK(caught);
      CHECK(v.GetSize() == 10);
      for (int k = 0; k < v.GetSize(); ++k)
        CHECK((t4_i64)pl(v[k]) == (t4_i64)k << 40);
      CHECK(!s.Commit());
      CHECK(s.Rollback());
    }
  }
  remove(fn);

  // a handler of our own, which is called before the error propagates
  {
    c4_SizeErrorHandler previous = c4_SetSizeErrorHandler(ThrowingHandler);
    f4_MaxColumnSize = 50;
    c4_Storage s;
    c4_View v = s.GetAs("a[b:B]");
    v.SetSize(1);
    static t4_byte big[51];
    int caught = 0;
    try {
      pb(v[0]).SetData(c4_Bytes(big, sizeof big));
    } catch (int limit) {
      caught = limit;
    }
    f4_MaxColumnSize = 0x7fffffff;
    CHECK(caught == 50 && sHandled == 1);
    CHECK(c4_SetSizeErrorHandler(previous) == ThrowingHandler);
    // and zero restores the default
    CHECK(c4_SetSizeErrorHandler(0) == previous);
  }
}
#endif 

static void TestColumnLimit() {
  printf("limit on the size of a column\n");

  // exactly at the limit is fine, this is committed and read back
  const char *fn = "twide_col.mk";
  remove(fn);
  f4_MaxColumnSize = 100000;
  {
    c4_Storage s(fn, 1);
    c4_View v = s.GetAs("a[i:I,s:S,b:B]");
    c4_BytesProp pb("b");
    static t4_byte buf[100000];
    for (int k = 0; k < 100000; ++k)
      buf[k] = (t4_byte)(k * 7);
    v.SetSize(1);
    pb(v[0]).SetData(c4_Bytes(buf, sizeof buf));
    CHECK(s.Commit());
  }
  {
    c4_Storage s(fn, 0);
    c4_View v = s.View("a");
    c4_BytesProp pb("b");
    CHECK(v.GetSize() == 1);
    c4_Bytes data;
    pb(v[0]).GetData(data);
    CHECK(data.Size() == 100000);
    bool same = data.Size() == 100000;
    for (int k = 0; same && k < 100000; ++k)
      same = data.Contents()[k] == (t4_byte)(k * 7);
    CHECK(same);
  }
  remove(fn);

  // changing part of an item (memo) refuses to grow it beyond the limit,
  // which can be reported, and leaves the item as it was
  {
    f4_MaxColumnSize = 50000;
    c4_Storage s;
    c4_View v = s.GetAs("a[b:B]");
    c4_BytesProp pb("b");
    static t4_byte buf[40000];
    memset(buf, 'a', sizeof buf);
    v.SetSize(1);
    pb(v[0]).SetData(c4_Bytes(buf, sizeof buf));

    t4_byte more[100];
    memset(more, 'z', sizeof more);
    CHECK(!pb(v[0]).Modify(c4_Bytes(more, 100), 49950));
    CHECK(pb(v[0]).GetSize() == 40000);
    CHECK(!pb(v[0]).Modify(c4_Bytes(more, 100), 40000, 20000));
    CHECK(pb(v[0]).GetSize() == 40000);
    CHECK(pb(v[0]).Modify(c4_Bytes(more, 100), 49900)); // ends at the limit
    CHECK(pb(v[0]).GetSize() == 50000);
    CHECK(pb(v[0]).Modify(c4_Bytes(more, 100), 100)); // in the middle
    CHECK(pb(v[0]).GetSize() == 50000);
  }
  f4_MaxColumnSize = 0x7fffffff;

#if HAVE_FORK
  // beyond the limit it is not possible to carry on, so it ends right there
  CHECK(Aborts(ManyStrings));
  CHECK(Aborts(HugeLongs));
  CHECK(Aborts(OneByteTooMany));
#else 
  printf("  (no fork on this platform, skipping the checks which abort)\n");
#endif 
}

/////////////////////////////////////////////////////////////////////////////
// 1. The same commits must give the same results with either kind of marks

static void TestParity(int mode_) {
  printf("parity of standard and wide marks, mode %d\n", mode_);

  const char *fnStd = "twide_std.mk";
  const char *fnWide = "twide_wide.mk";
  remove(fnStd);
  remove(fnWide);

  {
    c4_Storage s1(fnStd, mode_);
    c4_Storage s2(fnWide, mode_);
    c4_View v1 = s1.GetAs("a[i:I,s:S,b:B]");
    c4_View v2 = s2.GetAs("a[i:I,s:S,b:B]");

    for (int round = 1; round <= 9; ++round) {
      int kind = round % 3 == 0 ? 1: round % 4 == 0 ? 2: 0;

      f4_WideAbove = kStdLimit;
      Mutate(v1, round, kind);
      CHECK(s1.Commit());

      f4_WideAbove = 0; // all of these commits use wide marks
      Mutate(v2, round, kind);
      CHECK(s2.Commit());
      f4_WideAbove = kStdLimit;

      CHECK(LiveSignature(v1) == LiveSignature(v2));
    }
  }

  f4_WideAbove = kStdLimit;
  int rows1 = 0, rows2 = 0;
  long sig1 = Signature(fnStd, &rows1);
  long sig2 = Signature(fnWide, &rows2);
  CHECK(sig1 != -1 && sig1 == sig2 && rows1 == rows2 && rows1 > 0);

  // the wide file must have the wide marks at the end, and ...
  c4_Bytes data;
  CHECK(ReadFile(fnWide, data));
  int n = data.Size();
  const t4_byte *p = data.Contents();
  CHECK(n > 24);
  if (n > 24) {
    const t4_byte *t = p + n - 24;
    CHECK(t[0] == 0x82 && t[1] == 0 && t[2] == 0 && t[3] == 0);
    CHECK(Get32(t + 4) == (t4_fpos)(n - 24)); // the skip tail points to itself
    CHECK(t[8] == 0x83 && t[9] == 0 && t[10] == 0 && t[11] == 0);
    CHECK(Get32(t + 12) == 0); // no bits beyond 32 here
    CHECK(t[16] == 0x81 && t[17] == 0 && (t[18] != 0 || t[19] != 0));
    CHECK(Get32(t + 20) > 0 && Get32(t + 20) < (t4_fpos)n);
  }

  // ... the standard file must have the same marks as ever
  CHECK(ReadFile(fnStd, data));
  n = data.Size();
  p = data.Contents();
  CHECK(n > 16);
  if (n > 16) {
    CHECK(p[n - 16] == 0x80 && p[n - 15] == 0 && p[n - 14] == 0 && p[n - 13] ==
      0);
    CHECK(Get32(p + n - 12) == (t4_fpos)(n - 16));
    CHECK(p[n - 8] == 0x80 && Get32(p + n - 4) > 0);
  }
  // header of a wide file made in mode 1 is flagged as such, standard is not
  if (mode_ == 1) {
    CHECK(ReadFile(fnWide, data));
    CHECK(data.Size() > 8 && (data.Contents()[3] &0xC0) == 0x40);
    CHECK(ReadFile(fnStd, data));
    CHECK(data.Size() > 8 && data.Contents()[3] == 0);
  }

  remove(fnStd);
  remove(fnWide);
}

/////////////////////////////////////////////////////////////////////////////
// 2. A file may become wide at some point, and must stay readable

static void TestTransition(int mode_) {
  printf("file becoming wide, mode %d\n", mode_);

  const char *fn = "twide_trans.mk";
  remove(fn);

  long expect = 0;
  for (int round = 1; round <= 12; ++round) {
    // narrow, then wide, then back to the default limit, which must not
    // matter once the file has wide marks: it then has to stay that way
    f4_WideAbove = round < 5 ? kStdLimit: round < 9 ? 0 : kStdLimit;

    {
      c4_Storage s(fn, mode_);
      c4_View v = s.GetAs("a[i:I,s:S,b:B]");
      CHECK(LiveSignature(v) == expect || round == 1);
      Mutate(v, round, round % 3 == 0 ? 1: round % 4 == 0 ? 2: 0);
      CHECK(s.Commit());
      expect = LiveSignature(v);
    }

    f4_WideAbove = kStdLimit;
    CHECK(Signature(fn) == expect);

    c4_Bytes data;
    CHECK(ReadFile(fn, data));
    const int n = data.Size();
    if (n > 24) {
      const bool wide = data.Contents()[n - 8] == 0x81;
      CHECK(wide == (round >= 5)); // exactly from round 5 on
      if (!wide)
        CHECK(data.Contents()[n - 8] == 0x80);
    }
  }

  remove(fn);
}

/////////////////////////////////////////////////////////////////////////////
// 3. Saving to a stream and loading it back, with wide marks

class MemStream: public c4_Stream {
  public:
    t4_byte *_buf;
    int _size, _cap, _pos;

    MemStream(): _buf(0), _size(0), _cap(0), _pos(0){}
    ~MemStream() {
      free(_buf);
    }

    virtual int Read(void *p_, int n_) {
      if (n_ > _size - _pos)
        n_ = _size - _pos;
      if (n_ > 0) {
        memcpy(p_, _buf + _pos, n_);
        _pos += n_;
      }
      return n_ > 0 ? n_ : 0;
    }

    virtual bool Write(const void *p_, int n_) {
      if (_size + n_ > _cap) {
        _cap = (_size + n_) * 2;
        _buf = (t4_byte*)realloc(_buf, _cap);
      }
      memcpy(_buf + _size, p_, n_);
      _size += n_;
      return true;
    }
};

static void TestStream() {
  printf("stream with wide marks\n");

  c4_Storage s;
  c4_View v = s.GetAs("a[i:I,s:S,b:B]");
  Mutate(v, 1, 0);
  Mutate(v, 2, 0);
  const long expect = LiveSignature(v);

  for (int wide = 0; wide < 2; ++wide) {
    f4_WideAbove = wide ? 0 : kStdLimit;

    MemStream out;
    s.SaveTo(out);
    f4_WideAbove = kStdLimit;
    CHECK(out._size > 24);

    const t4_byte *p = out._buf;
    const int n = out._size;
    CHECK(p[2] == 0x1A);
    if (wide) {
      CHECK((p[3] &0xC0) == 0x40); // flagged as a wide header ...
      CHECK(n > 24 && p[n - 24] == 0x82 && p[n - 16] == 0x83 && p[n - 8] ==
        0x81); // ... and has the wide tails
    } else
      CHECK(p[3] == 0 && p[n - 16] == 0x80 && p[n - 8] == 0x80);

    // ... and the stream loads again, whichever kind it is
    MemStream in;
    in.Write(out._buf, out._size);
    c4_Storage s2;
    CHECK(s2.LoadFrom(in));
    CHECK(LiveSignature(s2.View("a")) == expect);
  }
}

/////////////////////////////////////////////////////////////////////////////
// 4. Marks at positions beyond 4 Gb, using a file which is not really there

class PretendFile: public c4_Strategy {
  public:
    struct Piece {
        t4_fpos _pos;
        t4_byte _data[24];
        int _len;
    };

    Piece _pieces[8];
    int _count;
    t4_fpos _size;

    PretendFile(): _count(0), _size(0){}

    void Put(t4_fpos pos_, const t4_byte *p_, int n_) {
      _pieces[_count]._pos = pos_;
      memcpy(_pieces[_count]._data, p_, n_);
      _pieces[_count]._len = n_;
      ++_count;
    }

    virtual bool IsValid()const {
      return true;
    }

    virtual t4_fpos FileSize() {
      return _size;
    }

    virtual int DataRead(t4_fpos pos_, void *buf_, int len_) {
      memset(buf_, 0, len_);
      for (int i = 0; i < _count; ++i) {
        const Piece &pc = _pieces[i];
        for (int k = 0; k < len_; ++k)
          if (pos_ + k >= pc._pos && pos_ + k < pc._pos + pc._len)
            ((t4_byte*)buf_)[k] = pc._data[pos_ + k - pc._pos];
      }
      return len_;
    }
};

static void Put32(t4_byte *p_, t4_fpos v_) {
  p_[0] = (t4_byte)(v_ >> 24);
  p_[1] = (t4_byte)(v_ >> 16);
  p_[2] = (t4_byte)(v_ >> 8);
  p_[3] = (t4_byte)v_;
}

// lay out a wide file, written out by hand from the description in marks.h
static void WideImage(PretendFile &f_, t4_fpos skip_, t4_fpos root_, int len_,
  t4_byte skipType_ = 0x82, t4_byte extType_ = 0x83, t4_byte commitType_ = 0x81)
  {
  t4_byte head[8] =  {
    'J', 'L', 0x1A, 0x40, 0xFF, 0xFF, 0xFF, 0xFF
  };
  f_.Put(0, head, 8);

  t4_byte tails[24];
  memset(tails, 0, sizeof tails);

  tails[0] = skipType_;
  Put32(tails + 4, skip_ &0xFFFFFFFF);

  tails[8] = extType_;
  tails[9] = (t4_byte)(skip_ >> 48);
  tails[10] = (t4_byte)(skip_ >> 40);
  tails[11] = (t4_byte)(skip_ >> 32);
  Put32(tails + 12, root_ >> 32);

  tails[16] = commitType_;
  tails[17] = (t4_byte)(len_ >> 16);
  tails[18] = (t4_byte)(len_ >> 8);
  tails[19] = (t4_byte)len_;
  Put32(tails + 20, root_ &0xFFFFFFFF);

  f_.Put(skip_, tails, 24);
  f_._size = skip_ + 24;
}

static void TestBeyond4Gb() {
  printf("marks beyond 4 Gb\n");

  const t4_fpos big[] =  {
    0x7fffffffLL + 100, // just over what standard marks can do
    0xFFFFFFF0LL, // around 4 Gb, positions with a zero low part
    0x100000000LL + 24, 0x1ABCDEF123LL, // 110 Gb
    0x00AB00001234ABCDLL // 48 Pb, which also has bits in 48..55
  };

  for (int i = 0; i < (int)(sizeof big / sizeof *big); ++i) {
    const t4_fpos skip = big[i];
    const t4_fpos roots[] =  {
      1234, 0x100000000LL, skip - 1000, 0x2FFFFFFFFLL, skip - 24
    };
    for (int j = 0; j < (int)(sizeof roots / sizeof *roots); ++j) {
      PretendFile f;
      WideImage(f, skip, roots[j], 0x123456);

      t4_fpos end = f.EndOfData(f.FileSize());
      CHECK(end == skip + 24);
      CHECK(f._rootPos == roots[j]);
      CHECK(f._rootLen == 0x123456);
      CHECK(f._baseOffset == 0);
    }
  }

  // damaged or confusing marks must be refused, not misread
  const t4_fpos skip = 0x300000000LL + 4096;
  const t4_fpos root = 0x280000000LL;
  {
    PretendFile f;
    WideImage(f, skip, root, 100, 0x82, 0x84); // wrong extension
    CHECK(f.EndOfData(f.FileSize()) < 0);
  }
  {
    PretendFile f;
    WideImage(f, skip, root, 100, 0x92); // wrong skip type
    CHECK(f.EndOfData(f.FileSize()) < 0);
  }
  {
    PretendFile f;
    WideImage(f, skip, root, 0); // no root length
    CHECK(f.EndOfData(f.FileSize()) < 0);
  }
  {
    // the skip tail points to the wrong place, so there is no header there
    PretendFile f;
    WideImage(f, skip, root, 100);
    f._pieces[1]._data[7] ^= 0x10;
    CHECK(f.EndOfData(f.FileSize()) < 0);
  }
  {
    // a header which is neither standard, old nor wide
    PretendFile f;
    WideImage(f, skip, root, 100);
    f._pieces[0]._data[3] = 0xC0;
    CHECK(f.EndOfData(f.FileSize()) < 0);
  }
  {
    // a standard header does work with wide tails (extend mode files)
    PretendFile f;
    WideImage(f, skip, root, 100);
    f._pieces[0]._data[3] = 0;
    CHECK(f.EndOfData(f.FileSize()) == skip + 24);
    CHECK(f._rootPos == root);
  }
}

/////////////////////////////////////////////////////////////////////////////
// 5. Interrupted commits, which must end up as the old or the new state

class CrashStrategy: public c4_FileStrategy {
  public:
    int _budget, _writes;

    CrashStrategy(int budget_): _budget(budget_), _writes(0){}

    virtual void DataWrite(t4_fpos pos_, const void *buf_, int len_) {
      ++_writes;
      if (_budget-- > 0)
        c4_FileStrategy::DataWrite(pos_, buf_, len_);
    }

    virtual void DataCommit(t4_fpos newSize_) {
      if (_budget > 0)
        c4_FileStrategy::DataCommit(newSize_);
    }
};

// In the order in which they can happen as a commit progresses: the old
// state, a file which cannot be read and looks empty, and the new state.
// Anything else would mean that the file was damaged.
enum {
  kOld, kEmpty, kNew, kDamaged
};

static int Classify(const char *fn_, long before_, long after_) {
  int rows = 0;
  long sig = Signature(fn_, &rows);
  if (sig == before_)
    return kOld;
  if (sig == after_)
    return kNew;
  return rows == 0 ? kEmpty: kDamaged;
}

static void TestCrash(bool wide_, int *outcomes_) {
  const char *fn = "twide_crash.mk";

  for (int mode = 1; mode <= 2; ++mode)
  for (int seqlen = 1; seqlen <= 5; ++seqlen)
  for (int kind = 0; kind < 3; ++kind) {
    long before = 0, after = 0;
    int w = 0, last = 0;

    for (int n =  - 1;; ++n) {
      remove(fn);
      f4_WideAbove = wide_ ? 0 : kStdLimit;
      {
        c4_Storage s(fn, mode);
        c4_View v = s.GetAs("a[i:I,s:S,b:B]");
        for (int r = 0; r < seqlen; ++r) {
          Mutate(v, r + 1, r % 3 == 1 ? 1: 0);
          s.Commit();
        }
      }
      before = Signature(fn);
      {
        CrashStrategy cs(n < 0 ? 1000000: n);
        cs.DataOpen(fn, 1);
        c4_Storage s(cs, false, mode);
        c4_View v = s.GetAs("a[i:I,s:S,b:B]");
        Mutate(v, seqlen + 1, kind);
        s.Commit();
        if (n < 0)
          w = cs._writes;
      }
      f4_WideAbove = kStdLimit;

      if (n < 0) {
        after = Signature(fn);
        continue;
      }

      const int c = Classify(fn, before, after);
      ++outcomes_[c];
      CHECK(c != kDamaged);
      CHECK(c != kEmpty); // the last complete commit is found again
      CHECK(c >= last || before == after); // never goes back in time
      if (c > last && c != kDamaged)
        last = c;
      if (n == 0)
        CHECK(c == kOld || before == after);
      // The file ends with what remains of the interrupted commit (unless
      // it got that far), and must accept further commits all the same.
      if (n < w) {
        f4_WideAbove = wide_ ? 0 : kStdLimit;
        for (int k = 0; k < 3; ++k) {
          long live = 0;
          {
            c4_Storage s(fn, mode);
            c4_View v = s.GetAs("a[i:I,s:S,b:B]");
            Mutate(v, 50 + k, k == 0 ? kind: k);
            CHECK(s.Commit());
            live = LiveSignature(v);
          }
          CHECK(Signature(fn) == live);
        }
        f4_WideAbove = kStdLimit;
      }

      if (n >= w)
        break;
    }

    // and when it is not interrupted at all, the new state must be there
    CHECK(Classify(fn, before, after) == kNew || before == after);
  }
  remove(fn);
}

/////////////////////////////////////////////////////////////////////////////

int main() {
  TestColumnLimit();
#if q4_EXCEPTIONS
  TestSizeErrors();
#endif 
  TestTruncation(1, false);
  TestTruncation(2, false);
  TestTruncation(1, true);
  TestTruncation(2, true);
  TestFailedCommit(1, false);
  TestFailedCommit(2, false);
  TestFailedCommit(1, true);
  TestFailedCommit(2, true);
  TestFailedCommitNoRollback(1, false);
  TestFailedCommitNoRollback(2, false);
  TestFailedCommitNoRollback(1, true);
  TestFailedCommitNoRollback(2, true);
  TestParity(1);
  TestParity(2);
  TestTransition(1);
  TestTransition(2);
  TestStream();
  TestBeyond4Gb();

  int std[4] =  {
    0, 0, 0, 0
  }, wide[4] =  {
    0, 0, 0, 0
  };
  printf("interrupted commits, standard marks\n");
  TestCrash(false, std);
  printf("interrupted commits, wide marks\n");
  TestCrash(true, wide);
  printf("  outcomes (old/empty/new/damaged): standard %d/%d/%d/%d, wide "
    "%d/%d/%d/%d\n", std[0], std[1], std[2], std[3], wide[0], wide[1], wide[2],
    wide[3]);

  CHECK(wide[kEmpty] == 0 && std[kEmpty] == 0);
  CHECK(wide[kDamaged] == 0 && std[kDamaged] == 0);

  printf("%d checks, %s\n", sChecks, sFailures == 0 ? "PASSED" : "FAILED");
  return sFailures == 0 ? 0 : 1;
}
