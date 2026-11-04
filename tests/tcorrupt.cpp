// tcorrupt.cpp -- Opening damaged datafiles must never crash or hang
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

// Takes valid datafiles (all kinds of properties, subviews, memos, both
// kinds of marks, both commit modes), damages them in random ways (bytes
// changed, ranges zeroed or filled, the end cut off, pieces swapped), opens
// the result and reads everything in it.  What it finds does not matter,
// what matters is that it is a database or nothing, and not a crash, a hang,
// or a memory error (this is meant to be run with AddressSanitizer too).
//
//   tcorrupt [first-seed [count]]    (default 1 to 300, or 1 to 5000 with
//                                     the environment variable TCORRUPT_LONG)
//   tcorrupt -v <seed>               say what is done to the datafile

#include "mk4.h"
#include "mk4io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern t4_fpos f4_WideAbove;

static bool sVerbose = false;

// a small generator, so the same seed gives the same damage everywhere
static unsigned long long sState;

static unsigned long Next() {
  sState ^= sState << 13;
  sState ^= sState >> 7;
  sState ^= sState << 17;
  return (unsigned long)(sState >> 11) & 0x7FFFFFFFUL;
}

static int Below(int n_) {
  return (int)(Next() % n_);
}

static void Seed(unsigned long seed_) {
  sState = seed_ * 2654435761ULL + 88172645463325252ULL;
  for (int i = 0; i < 10; ++i)
    Next();
}

// make a valid datafile of one of a number of kinds
static void MakeValid(const char *fn_, int kind_) {
  remove(fn_);
  f4_WideAbove = (kind_ & 1) ? 0 : 0x7fffffff - 64;
  const int mode = 1 + ((kind_ >> 1) & 1);

  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_BytesProp pb("b");
  c4_LongProp pl("l");
  c4_FloatProp pf("f");
  c4_DoubleProp pd("d");
  c4_ViewProp pv("v");

  c4_Storage s(fn_, mode);
  c4_View a = s.GetAs("a[i:I,s:S,b:B,l:L,f:F,d:D,v[x:I,y:S]]");
  c4_View c = s.GetAs("c[k:I]");
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 60; ++i) {
      char tmp[40];
      sprintf(tmp, "string %d.%d", round, i);
      int n = a.Add(pi[i * 7 - 100] + ps[tmp] + pl[(t4_i64)i << 33] + pf[i *
        0.5f] + pd[i * 1.25]);
      if (i % 9 == 0) {
        int len = 5000 + i * 37;
        t4_byte *b = (t4_byte*)malloc(len);
        for (int k = 0; k < len; ++k)
          b[k] = (t4_byte)(k * 3 + i);
        pb(a[n]).SetData(c4_Bytes(b, len));
        free(b);
      }
      c4_View sub = pv(a[n]);
      for (int k = 0; k < i % 4; ++k) {
        char t2[20];
        sprintf(t2, "sub%d", k);
        sub.Add(c4_IntProp("x")[k] + c4_StringProp("y")[t2]);
      }
    }
    for (int i = 0; i < 200; ++i)
      c.Add(c4_IntProp("k")[i * i]);
    if (round == 1)
      a.RemoveAt(5, 20);
    s.Commit();
  }
  f4_WideAbove = 0x7fffffff - 64;
}

// a datafile and an aside file with changes to it (see c4_Storage::SetAside)
static void MakeAside(const char *main_, const char *aside_) {
  remove(main_);
  remove(aside_);
  f4_WideAbove = 0x7fffffff - 64;

  {
    c4_Storage s(main_, 1);
    c4_View a = s.GetAs("a[i:I,s:S,b:B,v[x:I]]");
    for (int i = 0; i < 80; ++i) {
      char tmp[40];
      sprintf(tmp, "main %d", i);
      int n = a.Add(c4_IntProp("i")[i] + c4_StringProp("s")[tmp]);
      c4_View sub = c4_ViewProp("v")(a[n]);
      for (int k = 0; k < i % 3; ++k)
        sub.Add(c4_IntProp("x")[k]);
      if (i % 11 == 0) {
        t4_byte b[3000];
        for (int k = 0; k < 3000; ++k)
          b[k] = (t4_byte)(k + i);
        c4_BytesProp("b")(a[n]).SetData(c4_Bytes(b, 3000));
      }
    }
    s.Commit();
  }

  c4_Storage s1(main_, 0);
  c4_Storage s2(aside_, 1);
  s1.SetAside(s2);
  c4_View a = s1.View("a");
  for (int i = 0; i < 40; ++i) {
    char tmp[60];
    sprintf(tmp, "aside %d with some more text", i);
    int n = a.Add(c4_IntProp("i")[1000 + i] + c4_StringProp("s")[tmp]);
    c4_View sub = c4_ViewProp("v")(a[n]);
    sub.Add(c4_IntProp("x")[i]);
  }
  for (int i = 0; i < 80; i += 7)
    c4_StringProp("s")(a[i]) = "changed";
  a.RemoveAt(10, 12);
  t4_byte big[7000];
  for (int k = 0; k < 7000; ++k)
    big[k] = (t4_byte)(k * 5);
  c4_BytesProp("b")(a[3]).SetData(c4_Bytes(big, 7000));
  s1.Commit();
  s2.Commit();
}

static bool ReadFile(const char *fn_, std::string &data_) {
  FILE *f = fopen(fn_, "rb");
  if (f == 0)
    return false;
  data_.clear();
  char buf[65536];
  int n;
  while ((n = (int)fread(buf, 1, sizeof buf, f)) > 0)
    data_.append(buf, n);
  fclose(f);
  return true;
}

static void WriteFile(const char *fn_, const std::string &d_) {
  FILE *f = fopen(fn_, "wb");
  if (f == 0)
    exit(2);
  fwrite(d_.data(), 1, d_.size(), f);
  fclose(f);
}

// damage a datafile in one of a number of ways, most often near its end
// where the structure is (damage in the data itself cannot do much)
static void Damage(std::string &d_) {
  const int n = (int)d_.size();
  if (n < 32)
    return ;

  int count = 1 + Below(4);
  while (count-- > 0) {
    int where;
    switch (Below(4)) {
      case 0: where = n - 1 - Below(80 < n ? 80: n); break;
      case 1: where = n - 1 - Below(1000 < n ? 1000: n); break;
      case 2: where = Below(64 < n ? 64: n); break;
      default: where = Below(n);
    }
    switch (Below(7)) {
      case 0: // a byte changed
        d_[where] = (char)Next();
        if (sVerbose) printf("  set byte %d\n", where);
        break;
      case 1: // a bit flipped
        d_[where] ^= (char)(1 << Below(8));
        if (sVerbose) printf("  flip bit at %d\n", where);
        break;
      case 2: { // zeroed
        int len = 1 + Below(16);
        for (int k = 0; k < len && where + k < n; ++k)
          d_[where + k] = 0;
        if (sVerbose) printf("  zero %d bytes at %d\n", len, where);
        break;
      }
      case 3: { // set to 0xFF, which makes huge sizes
        int len = 1 + Below(8);
        for (int k = 0; k < len && where + k < n; ++k)
          d_[where + k] = (char)0xFF;
        if (sVerbose) printf("  fill %d bytes at %d with FF\n", len, where);
        break;
      }
      case 4: // cut off
        d_.resize(where > 8 ? where: 8);
        if (sVerbose) printf("  cut at %d\n", where);
        return ;
      case 5: { // copy a piece over another
        int len = 1 + Below(64);
        int from = Below(n);
        for (int k = 0; k < len && where + k < n && from + k < n; ++k)
          d_[where + k] = d_[from + k];
        if (sVerbose) printf("  copy %d bytes from %d to %d\n", len, from, where);
        break;
      }
      default: { // small values where a size might be
        int len = 1 + Below(4);
        for (int k = 0; k < len && where + k < n; ++k)
          d_[where + k] = (char)(0x80 + Below(3));
        if (sVerbose) printf("  small values at %d\n", where);
      }
    }
  }
}

// read everything in whatever got opened
static long Walk(c4_View v_, int depth_) {
  long sum = 0;
  if (depth_ > 4)
    return sum;

  const int rows = v_.GetSize();
  if (rows < 0)
    return sum;

  for (int i = 0; i < rows && i < 5000; ++i) {
    for (int p = 0; p < v_.NumProperties(); ++p) {
      const c4_Property &prop = v_.NthProperty(p);
      if (prop.Type() == 'V') {
        c4_View sub = ((const c4_ViewProp&)prop)(v_[i]);
        sum += Walk(sub, depth_ + 1);
      } else if (prop.Type() == 'S') {
        // as a C string, which must end where it is
        const char *str = ((const c4_StringProp&)prop)(v_[i]);
        sum += (long)strlen(str);
      } else {
        c4_Bytes b;
        if (v_.GetItem(i, p, b))
          for (int k = 0; k < b.Size(); k += 61)
            sum += b.Contents()[k];
      }
    }
  }
  return sum;
}

int main(int argc, char **argv) {
  unsigned long first = 1, count = getenv("TCORRUPT_LONG") != 0 ? 5000: 300;
  int a = 1;
  if (a < argc && strcmp(argv[a], "-v") == 0) {
    sVerbose = true;
    ++a;
    count = 1;
  }
  if (a < argc)
    first = strtoul(argv[a++], 0, 10);
  if (a < argc)
    count = strtoul(argv[a++], 0, 10);

  // the valid datafiles, made once
  const int kKinds = 4;
  std::string valid[kKinds];
  for (int k = 0; k < kKinds; ++k) {
    MakeValid("tcorrupt_src.mk", k);
    ReadFile("tcorrupt_src.mk", valid[k]);

    // a datafile which is not damaged must not be said to be
    c4_Storage s("tcorrupt_src.mk", 0);
    Walk(s.View("a"), 0);
    Walk(s.View("c"), 0);
    if (s.IsDamaged()) {
      printf("kind %d: a valid datafile is flagged as damaged\n", k);
      return 1;
    }
  }
  remove("tcorrupt_src.mk");

  // a string which does not end in a zero is not read beyond its end
  {
    remove("tcorrupt_str.mk");
    {
      c4_Storage s("tcorrupt_str.mk", 1);
      c4_View t = s.GetAs("t[s:S]");
      t.Add(c4_StringProp("s")["zzzzqqqq"]);
      s.Commit();
    }
    std::string d;
    ReadFile("tcorrupt_str.mk", d);
    size_t at = d.find("zzzzqqqq");
    if (at == std::string::npos) {
      printf("string test: the string is not in the datafile\n");
      return 1;
    }
    d[at + 8] = 'X';
    WriteFile("tcorrupt_str.mk", d);

    c4_Storage s("tcorrupt_str.mk", 0);
    c4_View tv = s.View("t");
    const char *str = c4_StringProp("s")(tv[0]);
    if (strlen(str) > 8) {
      printf("string test: a string without an end is read beyond it\n");
      return 1;
    }
    remove("tcorrupt_str.mk");
  }

  std::string asideMain, asideFile;
  MakeAside("tcorrupt_am.mk", "tcorrupt_aside.mk");
  ReadFile("tcorrupt_am.mk", asideMain);
  ReadFile("tcorrupt_aside.mk", asideFile);

  // and it must work as it is (this is how the datafile is not damaged)
  {
    c4_Storage s1("tcorrupt_am.mk", 0);
    c4_Storage s2("tcorrupt_aside.mk", 0);
    s1.SetAside(s2);
    if (s1.View("a").GetSize() != 80 + 40 - 12 || s1.IsDamaged() ||
      s2.IsDamaged()) {
      printf("the aside file does not work as it should\n");
      return 1;
    }
  }

  long total = 0;
  int opened = 0, invalid = 0, flagged = 0, asides = 0;
  for (unsigned long seed = first; seed < first + count; ++seed) {
    Seed(seed);

    if (seed % 4 == 3) {
      // an aside file which is damaged (or the datafile it is for)
      const bool main = Below(4) == 0;
      std::string d = main ? asideMain: asideFile;
      if (sVerbose)
        printf("seed %lu, aside scenario, %s damaged\n", seed, main ?
          "datafile": "aside file");
      Damage(d);
      WriteFile(main ? "tcorrupt_am.mk": "tcorrupt_aside.mk", d);
      WriteFile(main ? "tcorrupt_aside.mk": "tcorrupt_am.mk", main ? asideFile:
        asideMain);

      c4_Storage s1("tcorrupt_am.mk", 0);
      c4_Storage s2("tcorrupt_aside.mk", 0);
      if (s1.Strategy().IsValid() && s2.Strategy().IsValid()) {
        s1.SetAside(s2);
        total += Walk(s1.View("a"), 0);
        total += Walk(s1, 0);
        ++asides;
      }
      continue;
    }

    const int kind = Below(kKinds);
    std::string d = valid[kind];
    if (sVerbose)
      printf("seed %lu, kind %d, %d bytes\n", seed, kind, (int)d.size());
    Damage(d);
    WriteFile("tcorrupt_dmg.mk", d);

    {
      c4_Storage s("tcorrupt_dmg.mk", 0);
      if (!s.Strategy().IsValid()) {
        ++invalid;
        continue;
      }
      ++opened;
      total += Walk(s.View("a"), 0);
      total += Walk(s.View("c"), 0);
      total += Walk(s, 0);
      if (s.IsDamaged())
        ++flagged;
    }

    // and to change it, the same must hold
    if (seed % 5 == 0) {
      WriteFile("tcorrupt_fail.mk", d); // (kept if something is wrong)
      c4_Storage s("tcorrupt_dmg.mk", 1);
      // (a property has the one type it had in the datafile: a name which
      // is not used is taken, damage can have changed what a name is)
      if (s.FindPropIndexByName("added") < 0) {
        c4_View c = s.GetAs("added[k:I]");
        c.Add(c4_IntProp("k")[1]);
      }
      s.Commit();
    }

    // and what has been committed after that is sound
    if (seed % 5 == 0) {
      c4_Storage s("tcorrupt_dmg.mk", 0);
      if (!s.Strategy().IsValid() || s.IsDamaged()) {
        printf("seed %lu: after a commit the datafile is %s (tcorrupt_fail.mk"
          " is what it was before)\n", seed, s.Strategy().IsValid() ?
          "still damaged": "not valid");
        return 1;
      }
      if (s.FindPropIndexByName("added") >= 0)
        total += Walk(s.View("added"), 0);
      total += Walk(s, 0);
      remove("tcorrupt_fail.mk");
    }
  }
  remove("tcorrupt_dmg.mk");
  remove("tcorrupt_am.mk");
  remove("tcorrupt_aside.mk");

  printf("%lu damaged datafiles: %d opened (%d found damaged), %d invalid, "
    "%d with an aside file, no crash (%ld)\n", count, opened, flagged, invalid,
    asides, total & 0xFF);
  return 0;
}
