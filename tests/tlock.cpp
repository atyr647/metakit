// tlock.cpp -- Test program for writers which use one datafile at the same time
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

// Two programs which have a datafile open for writing used to replace each
// other's commits, and both were told that theirs had succeeded.  A writer now
// takes a lock on the file (see c4_FileStrategy::TakeLock), and if it was told
// not to, a commit is refused when the file is not as it was when this program
// read it.  These tests make sure that no commit which was reported as made is
// ever lost, with the lock and without it, and that the lock is let go of
// when the program which has it is killed.
//
// The parts with more than one program need fork, so they run on Unix.
//
// This program is built and run by "make test" on Unix, next to regress.

#include "mk4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined (unix) || defined (__unix__) || defined (__APPLE__)
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#define HAVE_FORK 1
#endif

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(c) do { ++sChecks; if (!(c)) { \
    fprintf(stderr, "CHECK FAILED line %d: %s\n", __LINE__, #c); \
    ++sFailures; } } while (0)

static const char *kStructure = "a[i:I,s:S]";
static const char *kFile = "tlock.mk";

// a datafile with n rows, numbered 0 to n - 1
static void MakeFile(int n_) {
  remove(kFile);
  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_Storage s(kFile, 1);
  c4_View v = s.GetAs(kStructure);
  for (int i = 0; i < n_; ++i)
    v.Add(pi[i] + ps["a row which is there from the start"]);
  CHECK(s.Commit());
}

static void AddRows(c4_Storage &s_, int first_, int count_) {
  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_View v = s_.View("a");
  for (int i = 0; i < count_; ++i)
    v.Add(pi[first_ + i] + ps["a row"]);
}

// what is in the datafile, as a program which only reads sees it: the number
// of rows, and the sum of their numbers
static int Rows(long *sum_ = 0) {
  c4_Storage s(kFile, 0);
  c4_View v = s.View("a");
  c4_IntProp pi("i");
  if (sum_ != 0) {
    *sum_ = 0;
    for (int i = 0; i < v.GetSize(); ++i)
      *sum_ += pi(v[i]);
  }
  return v.GetSize();
}

/////////////////////////////////////////////////////////////////////////////
// 1. The lock, in one program

static void TestLock() {
  printf("a second writer is kept out\n");
  MakeFile(10);

  {
    c4_Storage a(kFile, 1);
    CHECK(a.Strategy().IsValid());
    CHECK(a.Strategy()._failure == 0);

    // the same program, a second writer, in either mode
    for (int mode = 1; mode <= 2; ++mode) {
      c4_Storage b(kFile, mode);
      CHECK(!b.Strategy().IsValid());
      CHECK(b.Strategy()._failure == c4_Strategy::kFailLocked);
    }

    // readers are not in the way, and see what is committed
    c4_Storage r(kFile, 0);
    CHECK(r.Strategy().IsValid());
    CHECK(r.View("a").GetSize() == 10);

    // and the writer carries on
    AddRows(a, 100, 2);
    CHECK(a.Commit());
    c4_Storage r2(kFile, 0);
    CHECK(r2.View("a").GetSize() == 12);
  }

  // the lock goes when the file is closed
  {
    c4_Storage b(kFile, 1);
    CHECK(b.Strategy().IsValid());
    AddRows(b, 200, 1);
    CHECK(b.Commit());
  }
  CHECK(Rows() == 13);

  // a file which is made new is locked as well
  remove(kFile);
  {
    c4_Storage a(kFile, 1);
    CHECK(a.Strategy().IsValid());
    c4_Storage b(kFile, 1);
    CHECK(!b.Strategy().IsValid());
    CHECK(b.Strategy()._failure == c4_Strategy::kFailLocked);
  }

  printf("the lock can be left out\n");
  MakeFile(10);
  {
    c4_Storage a(kFile, 1);
    c4_Storage b(kFile, 1, false); // no lock for this one, but a has it
    CHECK(b.Strategy().IsValid());
  }
  {
    c4_Storage a(kFile, 1, false);
    c4_Storage b(kFile, 1);
    CHECK(b.Strategy().IsValid()); // there was no lock to be kept out by
  }
#if HAVE_FORK
  {
    c4_Storage a(kFile, 1);
    setenv("METAKIT_NOLOCK", "1", 1);
    c4_Storage b(kFile, 1);
    CHECK(b.Strategy().IsValid());
    setenv("METAKIT_NOLOCK", "0", 1);
    c4_Storage c(kFile, 1);
    CHECK(!c.Strategy().IsValid()); // 0 is not a yes
    unsetenv("METAKIT_NOLOCK");
  }
#endif
}

/////////////////////////////////////////////////////////////////////////////
// 2. Without the lock, what a program has not seen is not replaced

static void TestStale(int mode_) {
  printf("a commit which would replace another one is refused, mode %d\n",
    mode_);

  // both programs have read the file, then one commits, then the other
  MakeFile(10);
  {
    c4_Storage a(kFile, mode_, false);
    c4_Storage b(kFile, mode_, false);
    AddRows(a, 100, 3);
    AddRows(b, 200, 5);

    CHECK(a.Commit());
    CHECK(!b.Commit()); // this used to say yes, and lose the 3 rows of a
    CHECK(b.Strategy()._failure == c4_Strategy::kFailChanged);
    CHECK(Rows() == 13);

    // it is refused again, nothing was written, nothing is lost
    CHECK(!b.Commit());
    CHECK(b.Strategy()._failure == c4_Strategy::kFailChanged);
    CHECK(Rows() == 13);

    // after loading what is there, the changes can be made again
    CHECK(b.Rollback());
    CHECK(b.View("a").GetSize() == 13);
    AddRows(b, 200, 5);
    CHECK(b.Commit());
    CHECK(b.Strategy()._failure == 0);
    // and then the other one is the one which is behind
    AddRows(a, 300, 1);
    CHECK(!a.Commit());
  }
  long sum = 0;
  CHECK(Rows(&sum) == 18);
  CHECK(sum == 45 + 100 + 101 + 102 + 200 + 201 + 202 + 203 + 204);

  // a program which has not changed anything has nothing to refuse
  {
    c4_Storage a(kFile, mode_, false);
    c4_Storage b(kFile, mode_, false);
    AddRows(a, 400, 1);
    CHECK(a.Commit());
    CHECK(b.Commit());
    CHECK(b.Strategy()._failure == 0);
  }
  CHECK(Rows() == 19);

  // changes which do not change the size of the file (they are made in
  // place) are seen too
  MakeFile(10);
  {
    c4_IntProp pi("i");
    c4_Storage a(kFile, mode_, false);
    c4_Storage b(kFile, mode_, false);
    c4_View va = a.View("a"), vb = b.View("a");
    pi(va[3]) = 111;
    pi(vb[4]) = 222;
    CHECK(a.Commit());
    CHECK(!b.Commit());
  }
  {
    c4_Storage r(kFile, 0);
    c4_View v = r.View("a");
    c4_IntProp pi("i");
    CHECK(pi(v[3]) == 111);
    CHECK(pi(v[4]) == 4);
  }

  // a file which was not there: the first to commit gets it
  remove(kFile);
  {
    c4_Storage a(kFile, mode_, false);
    c4_Storage b(kFile, mode_, false);
    c4_View va = a.GetAs(kStructure), vb = b.GetAs(kStructure);
    AddRows(a, 1, 2);
    AddRows(b, 5, 3);
    CHECK(b.Commit());
    CHECK(!a.Commit());
  }
  CHECK(Rows() == 3);
}

/////////////////////////////////////////////////////////////////////////////
// 3. More than one program

#if HAVE_FORK

// one byte between programs
static void Put(int fd_, char c_) {
  if (write(fd_, &c_, 1) != 1)
    _exit(9);
}

static char Get(int fd_) {
  char c = 0;
  return read(fd_, &c, 1) == 1 ? c: 0;
}

// wait for a child, and say how it ended: its exit status, or - the signal
static int Wait(pid_t pid_) {
  int status = 0;
  waitpid(pid_, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status):  - WTERMSIG(status);
}

static void TestKilled() {
  printf("the lock is let go of when its program is killed\n");
  MakeFile(10);

  int fds[2];
  CHECK(pipe(fds) == 0);
  pid_t pid = fork();
  if (pid == 0) {
    c4_Storage s(kFile, 1);
    Put(fds[1], s.Strategy().IsValid() ? 'y': 'n');
    // stay, with the file open and possibly in the middle of a change
    AddRows(s, 100, 1);
    for (;;)
      pause();
  }

  CHECK(Get(fds[0]) == 'y');
  {
    c4_Storage b(kFile, 1);
    CHECK(!b.Strategy().IsValid());
    CHECK(b.Strategy()._failure == c4_Strategy::kFailLocked);
    CHECK(Rows() == 10); // readers are fine
  }

  kill(pid, SIGKILL);
  CHECK(Wait(pid) ==  - SIGKILL);

  // no waiting, no stale lock file to take away
  {
    c4_Storage b(kFile, 1);
    CHECK(b.Strategy().IsValid());
    AddRows(b, 200, 1);
    CHECK(b.Commit());
  }
  CHECK(Rows() == 11);
  close(fds[0]);
  close(fds[1]);
}

// a writer which waits for its turn, as mk::transaction does
static int Turns(int who_, int count_) {
  for (int i = 0; i < count_; ++i) {
    int tries = 0;
    for (;;) {
      c4_Storage s(kFile, 1);
      if (s.Strategy().IsValid()) {
        AddRows(s, who_ * 1000 + i, 1);
        if (!s.Commit())
          return 1;
        break;
      }
      if (s.Strategy()._failure != c4_Strategy::kFailLocked || ++tries > 5000)
        return 2;
      usleep(1000);
    }
  }
  return 0;
}

static void TestSerialised() {
  printf("writers which take turns lose nothing\n");
  MakeFile(10);

  pid_t pids[3];
  for (int w = 0; w < 3; ++w)
    if ((pids[w] = fork()) == 0)
      _exit(Turns(w + 1, 20));

  for (int w = 0; w < 3; ++w)
    CHECK(Wait(pids[w]) == 0);

  long sum = 0;
  CHECK(Rows(&sum) == 10 + 60);
  long expect = 45;
  for (int w = 1; w <= 3; ++w)
    for (int i = 0; i < 20; ++i)
      expect += w * 1000 + i;
  CHECK(sum == expect);
}

// the old way, which lost commits, with the lock left out: one of them is told
static void TestTwoPrograms() {
  printf("two programs without the lock: one is refused\n");
  MakeFile(10);

  int up[2], go[2];
  CHECK(pipe(up) == 0 && pipe(go) == 0);

  c4_Storage a(kFile, 1, false);
  pid_t pid = fork();
  if (pid == 0) {
    // the other program has read the file as well, and waits to be told
    c4_Storage b(kFile, 1, false);
    Put(up[1], 'x');
    Get(go[0]);
    AddRows(b, 200, 5);
    // 0: committed, 3: refused as it should be, 4: anything else
    _exit(b.Commit() ? 0: b.Strategy()._failure == c4_Strategy::kFailChanged ?
      3: 4);
  }

  CHECK(Get(up[0]) == 'x');
  AddRows(a, 100, 3);
  CHECK(a.Commit());
  Put(go[1], 'g');
  CHECK(Wait(pid) == 3);

  CHECK(Rows() == 13); // the 3 rows were not lost
  close(up[0]);
  close(up[1]);
  close(go[0]);
  close(go[1]);
}

#endif // HAVE_FORK

/////////////////////////////////////////////////////////////////////////////

int main() {
#if HAVE_FORK
  unsetenv("METAKIT_NOLOCK");
#endif
  TestLock();
  TestStale(1);
  TestStale(2);
#if HAVE_FORK
  TestKilled();
  TestSerialised();
  TestTwoPrograms();
#endif
  remove(kFile);

  printf("%d checks, %s\n", sChecks, sFailures == 0 ? "PASSED" : "FAILED");
  return sFailures == 0 ? 0 : 1;
}
