// compat.cpp -- Helper for compat.sh, which compares two versions of Metakit
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

// This uses nothing but the public interface, so that it can be built with
// an old version of Metakit as well as with the current one.
//
//   compat gen <directory>   make a number of datafiles (m1.mk, m2.mk)
//   compat digest <file> [rw] print what is in a datafile, opened read-only
//                            (or for changes, but then nothing is committed)
//   compat append <file>     add rows to a datafile and commit, as a program
//                            which does not know any better would do

#include "mk4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kStructure = "a[i:I,s:S,b:B,l:L]";

static void Generate(const char *dir_) {
  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_BytesProp pb("b");
  c4_LongProp pl("l");

  for (int mode = 1; mode <= 2; ++mode) {
    char fn[512];
    sprintf(fn, "%s/m%d.mk", dir_, mode);
    remove(fn);

    c4_Storage s(fn, mode);
    c4_View v = s.GetAs(kStructure);
    for (int round = 0; round < 6; ++round) {
      for (int i = 0; i < 700; ++i) {
        char tmp[64];
        sprintf(tmp, "str-%d-%d", round, i);
        int n = v.Add(pi[i * round] + ps[tmp]);
        if (i % 50 == 0) {
          int len = 70000 + i * 13;
          t4_byte *b = (t4_byte*)malloc(len);
          for (int k = 0; k < len; ++k)
            b[k] = (t4_byte)(k + i + round);
          pb(v[n]).SetData(c4_Bytes(b, len));
          free(b);
        }
        pl(v[n]) = ((t4_i64)i << 33) + round;
      }
      if (round % 2)
        v.RemoveAt(10, 300);
      if (round == 3) {
        pi(v[5]) = 999999;
        ps(v[7]) = "changed";
      }
      if (!s.Commit()) {
        fprintf(stderr, "commit failed\n");
        exit(1);
      }
    }
  }
}

static void Digest(const char *fn_, bool rw_) {
  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_BytesProp pb("b");
  c4_LongProp pl("l");

  c4_Storage s(fn_, rw_ ? 1: 0);
  c4_View v = s.GetAs(kStructure);

  unsigned long h = 5381;
  for (int i = 0; i < v.GetSize(); ++i) {
    h = h * 33 + (unsigned long)(long)pi(v[i]);
    for (const char *q = ps(v[i]); *q; ++q)
      h = h * 33 + (unsigned char)*q;
    c4_Bytes b;
    pb(v[i]).GetData(b);
    for (int k = 0; k < b.Size(); k += 61)
      h = h * 33 + b.Contents()[k];
    h = h * 33 + b.Size();
    h = h * 33 + (unsigned long)(t4_i64)pl(v[i]);
  }
  printf("rows=%d digest=%lx\n", v.GetSize(), h & 0xFFFFFFFFUL);
}

static void Append(const char *fn_) {
  c4_IntProp pi("i");
  c4_StringProp ps("s");

  c4_Storage s(fn_, 1);
  c4_View v = s.GetAs(kStructure);
  for (int i = 0; i < 10; ++i)
    v.Add(pi[i] + ps["appended"]);
  printf("commit %s, now %d rows\n", s.Commit() ? "ok": "FAILED", v.GetSize());
}

int main(int argc, char **argv) {
  if (argc >= 3 && strcmp(argv[1], "gen") == 0)
    Generate(argv[2]);
  else if (argc >= 3 && strcmp(argv[1], "digest") == 0)
    Digest(argv[2], argc > 3 && strcmp(argv[3], "rw") == 0);
  else if (argc >= 3 && strcmp(argv[1], "append") == 0)
    Append(argv[2]);
  else {
    fprintf(stderr, "usage: compat gen <dir> | digest <file> [rw] | append "
      "<file>\n");
    return 2;
  }
  return 0;
}
