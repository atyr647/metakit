// mkinfo.cpp -- Tell what kind of Metakit datafile a file is, without Metakit
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

// Usage: mkinfo [-q] <file>...
//
// This reads the marks at the start and the end of a datafile (see
// src/marks.h for what they are), using code of its own, so it can be used
// before opening a file with the library, and without it.  The most
// important thing it reports is whether the datafile needs this version of
// Metakit (or later): datafiles which have grown over 2 GB are written with
// "wide" marks, which versions before that do not understand.  They treat
// such a file as an empty database, and a program which then commits
// something puts a new datafile behind the existing one, hiding the data.
//
// The exit status is 0 if every file can be used by all versions of Metakit,
// 3 if one of them needs the wide marks, 1 if one of them is not a datafile
// (or cannot be read), and 2 for a wrong command line.  With -q, only a
// single word is printed for each file: standard, wide, old, empty, damaged
// (it has data, but is not complete) or unknown.

#if !defined (_WIN32)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif 
#endif 

#include <stdio.h>
#include <string.h>

typedef long long i64;

#if defined (_WIN32) && defined (_MSC_VER) && _MSC_VER >= 1400
#define seek64(f,p) _fseeki64(f, (__int64)(p), 0)
#define seekend64(f) _fseeki64(f, 0, 2)
#define tell64(f) ((i64)_ftelli64(f))
#elif defined (_WIN32) && defined (__MINGW32__)
#define seek64(f,p) fseeko64(f, (off64_t)(p), 0)
#define seekend64(f) fseeko64(f, 0, 2)
#define tell64(f) ((i64)ftello64(f))
#elif defined (_WIN32)
#define seek64(f,p) fseek(f, (long)(p), 0)
#define seekend64(f) fseek(f, 0, 2)
#define tell64(f) ((i64)ftell(f))
#else 
#define seek64(f,p) fseeko(f, (off_t)(p), 0)
#define seekend64(f) fseeko(f, 0, 2)
#define tell64(f) ((i64)ftello(f))
#endif 

static bool Read(FILE *f_, i64 pos_, unsigned char *buf_, int len_) {
  return pos_ >= 0 && seek64(f_, pos_) == 0 && (int)fread(buf_, 1, len_, f_) ==
    len_;
}

static i64 Get32(const unsigned char *p_) {
  return ((i64)p_[0] << 24) + (p_[1] << 16) + (p_[2] << 8) + p_[3];
}

static bool IsHeader(const unsigned char *m_) {
  return (m_[0] == 'J' || m_[0] == 'L') && (m_[0] ^ m_[1]) == ('J' ^ 'L') &&
    m_[2] == 0x1A && (m_[3] &0xC0) != 0xC0;
}

struct Info {
    bool _wide, _old, _found;
    i64 _end, _rootPos;
    int _rootLen;
    bool _bigEndian;
};

// look for tails which end at the given position, as the library does
static bool Scan(FILE *f_, i64 end_, Info &info_) {
  unsigned char m[8], x[8], s[8], h[8];
  if (end_ < 24 || !Read(f_, end_ - 8, m, 8))
    return false;

  const int len = (m[1] << 16) + (m[2] << 8) + m[3];
  i64 skipPos = 0, skipAt = 0;
  bool wide = false;

  if (m[0] == 0x80 && len > 0 && Get32(m + 4) > 0) {
    skipAt = end_ - 16;
    if (!Read(f_, skipAt, s, 8) || !((s[0] &0xF0) == 0x90 || s[0] == 0x80) ||
      (s[0] == 0x80 && (s[1] | s[2] | s[3]) != 0) || Get32(s + 4) <= 0)
      return false;
    skipPos = Get32(s + 4);
    info_._rootPos = Get32(m + 4);
  } else if (m[0] == 0x81 && len > 0) {
    wide = true;
    skipAt = end_ - 24;
    if (!Read(f_, end_ - 16, x, 8) || x[0] != 0x83 || !Read(f_, skipAt, s, 8) ||
      s[0] != 0x82 || (s[1] | s[2] | s[3]) != 0)
      return false;
    skipPos = (((i64)x[1] << 16) + (x[2] << 8) + x[3]) * 4294967296LL +
      Get32(s + 4);
    info_._rootPos = Get32(x + 4) * 4294967296LL + Get32(m + 4);
  } else
    return false;

  // the skip tail leads back to the header
  const i64 hpos = skipAt - skipPos;
  if (skipPos <= 0 || info_._rootPos <= 0 || !Read(f_, hpos, h, 8) ||
    !IsHeader(h))
    return false;

  info_._wide = wide;
  info_._end = end_;
  info_._rootLen = len;
  info_._bigEndian = h[0] == 'L';
  info_._found = true;
  return true;
}

static int Examine(const char *fn_, bool quiet_) {
  FILE *f = fopen(fn_, "rb");
  if (f == 0) {
    if (quiet_)
      printf("unknown\n");
    else
      printf("%s: cannot be opened\n", fn_);
    return 1;
  }

  seekend64(f);
  const i64 size = tell64(f);

  const char *word = "unknown";
  int status = 1;
  Info info;
  memset(&info, 0, sizeof info);
  unsigned char h[8];

  if (size == 0) {
    word = "empty";
    status = 0;
  } else if (size >= 8 && Read(f, 0, h, 8) && IsHeader(h) && h[3] == 0x80) {
    word = "old"; // before version 2, the header has the position, see viewx.cpp
    status = 0;
  } else if (size >= 24 && Scan(f, size, info)) {
    word = info._wide ? "wide" : "standard";
    status = info._wide ? 3 : 0;
  } else if (size >= 8 && Read(f, 0, h, 8) && IsHeader(h)) {
    // there is a header: the last commit may not be the one which was
    // completed, or only the first one is (as in the middle of a commit)
    word = "damaged";
    status = 1;
    i64 next = Get32(h + 4);
    if (h[3] &0x40) {
      next += (i64)(h[3] &0x3F) * 4294967296LL;
      if (h[3] == 0x7F && Get32(h + 4) == 0xFFFFFFFFLL)
        next = 0;
    }
    Info last;
    memset(&last, 0, sizeof last);
    // follow what the header says (see c4_Strategy::RecoverEnd) at least once
    for (int n = 0; n < 1000000 && next > 24 && next <= size; ++n) {
      Info cur;
      memset(&cur, 0, sizeof cur);
      if (!Scan(f, next, cur))
        break;
      last = cur;
      unsigned char m[8];
      // the mark which each commit leaves where the one before ended is like a
      // header, except that the third byte is 0A
      if (next + 8 > size || !Read(f, next, m, 8) || (m[0] != 'J' && m[0] !=
        'L') || (m[0] ^ m[1]) != ('J' ^ 'L') || m[2] != 0x0A || (m[3] &0x80))
        break;
      i64 step = Get32(m + 4);
      if (m[3] &0x40)
        step += (i64)(m[3] &0x3F) * 4294967296LL;
      if (step <= 0)
        break;
      next += step;
    }
    if (last._found) {
      info = last;
      info._found = true;
      word = last._wide ? "damaged-wide" : "damaged";
      status = 1;
    }
  } else {
    word = "unknown";
    status = 1;
  }
  fclose(f);

  if (quiet_) {
    printf("%s\n", strncmp(word, "damaged", 7) == 0 ? "damaged": word);
    return status;
  }

  printf("%s: %lld bytes\n", fn_, size);
  if (strcmp(word, "empty") == 0)
    printf("  empty file, no Metakit datafile (yet)\n");
  else if (strcmp(word, "old") == 0)
    printf("  Metakit datafile in the format before version 2\n  readable by"
      " all versions\n");
  else if (strcmp(word, "unknown") == 0)
    printf("  not a Metakit datafile\n");
  else {
    if (info._found)
      printf("  last complete commit ends at %lld, root of %d bytes at %lld,"
        " %s\n", info._end, info._rootLen, info._rootPos, info._bigEndian ?
        "big-endian": "little-endian");
    if (strncmp(word, "damaged", 7) == 0)
      printf("  the datafile does not end with a complete commit%s\n",
        info._found ? ", the one above is the last which is": ", and none was"
        " found");
    if (info._found && info._end < size)
      printf("  %lld bytes at the end are not part of it\n", size - info._end);
    if (info._wide)
      printf("  WIDE MARKS: this datafile needs Metakit with support for "
        "datafiles beyond 2 GB.\n  Older versions see it as EMPTY, and a "
        "program using one to add data would put\n  a new datafile behind this"
        " one and hide it.  Do not open it with those.\n");
    else if (info._found && strcmp(word, "standard") == 0)
      printf("  standard marks: readable by all versions\n");
    else if (info._found)
      printf("  that commit has standard marks, which all versions can read,"
        " but this\n  datafile is not complete as it is\n");
  }
  return status;
}

int main(int argc, char **argv) {
  bool quiet = false;
  int first = 1;
  if (argc > 1 && strcmp(argv[1], "-q") == 0) {
    quiet = true;
    first = 2;
  }
  if (first >= argc) {
    fprintf(stderr, "usage: mkinfo [-q] <file>...\n");
    return 2;
  }

  int result = 0;
  for (int i = first; i < argc; ++i) {
    int status = Examine(argv[i], quiet);
    // 1 (not usable) outranks 3 (needs a recent version) outranks 0
    if (status == 1 || (status == 3 && result == 0))
      result = status;
  }
  return result;
}
