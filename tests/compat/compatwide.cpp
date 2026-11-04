// compatwide.cpp -- Helper for compat.sh: make a datafile with wide marks
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

// Uses the hook which makes a small datafile use the marks which are meant
// for datafiles beyond 2 Gb, see src/marks.h.  Needs the current Metakit.

#include "mk4.h"
#include <stdio.h>
#include <stdlib.h>

extern t4_fpos f4_WideAbove;

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;

  f4_WideAbove = 0;
  remove(argv[1]);

  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_BytesProp pb("b");
  c4_LongProp pl("l");

  c4_Storage s(argv[1], 1);
  c4_View v = s.GetAs("a[i:I,s:S,b:B,l:L]");
  for (int round = 0; round < 4; ++round) {
    for (int i = 0; i < 300; ++i) {
      char tmp[64];
      sprintf(tmp, "wide-%d-%d", round, i);
      int n = v.Add(pi[i * round] + ps[tmp]);
      pl(v[n]) = ((t4_i64)i << 33) + round;
      if (i % 50 == 0) {
        t4_byte b[20000];
        for (int k = 0; k < (int)sizeof b; ++k)
          b[k] = (t4_byte)(k + i);
        pb(v[n]).SetData(c4_Bytes(b, sizeof b));
      }
    }
    if (!s.Commit())
      return 1;
  }
  printf("wrote %d rows\n", v.GetSize());
  return 0;
}
