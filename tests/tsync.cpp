// tsync.cpp -- Makes commits, for tests/tsync.sh to look at their system calls
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

//   tsync <file> <mode> <commits> [wide]
//
// It writes a line to standard error before each commit, so that the commits
// can be told apart in the output of strace.

#include "mk4.h"
#include <stdio.h>
#include <stdlib.h>

extern t4_fpos f4_WideAbove;

int main(int argc, char **argv) {
  if (argc < 4)
    return 2;
  if (argc > 4)
    f4_WideAbove = 0;

  const int mode = atoi(argv[2]);
  const int commits = atoi(argv[3]);
  c4_IntProp pi("i");
  c4_StringProp ps("s");

  c4_Storage s(argv[1], mode);
  c4_View v = s.GetAs("a[i:I,s:S]");
  for (int n = 1; n <= commits; ++n) {
    char text[40];
    sprintf(text, "row %d of the test", n);
    v.Add(pi[n] + ps[text]);
    fprintf(stderr, "COMMIT %d\n", n);
    if (!s.Commit()) {
      fprintf(stderr, "commit failed\n");
      return 1;
    }
  }
  return 0;
}
