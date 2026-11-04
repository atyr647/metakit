// tfuzz.cpp -- Randomized fault tests, checked against a model
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

// Each seed makes a scenario: a sequence of random changes to a database,
// mixed with commits, rollbacks, reopening, commits which are made to fail,
// commits which are interrupted after a random number of writes, and copies
// of the datafile which are cut short.  A model of the contents is kept next
// to the real thing, and every time the two are compared in full, from the
// datafile as it was persisted and not from what a commit returned.
//
// The scenario depends on the seed alone, so a failure can be repeated:
//
//   tfuzz [first-seed [count]]    (default: seeds 1 to 40, or 1 to 1000 with
//                                  the environment variable TFUZZ_LONG set)
//   tfuzz -v <seed>               show every step of one scenario
//
// This uses the hooks in src/marks.h, see twide.cpp for what they are.

#include "mk4.h"
#include "mk4io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern t4_fpos f4_WideAbove;
extern t4_fpos f4_MaxFileEnd;

static const t4_fpos kStdLimit = 0x7fffffff - 64;
static const t4_fpos kMaxEndDefault = f4_MaxFileEnd;

static bool sVerbose = false;
static unsigned long sSeed = 0;
static int sStep = 0;
static int sFailures = 0;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "seed %lu step %d: CHECK FAILED line %d: %s\n", sSeed, \
      sStep, __LINE__, #c); \
    ++sFailures; throw 1; } } while (0)

#define LOG if (sVerbose) printf

/////////////////////////////////////////////////////////////////////////////
// a small random number generator, so scenarios are the same everywhere

class Random {
    unsigned long long _s;
  public:
    Random(unsigned long seed_): _s(seed_ * 2654435761ULL + 88172645463325252ULL)
      {
      for (int i = 0; i < 10; ++i)
        Next();
    }

    unsigned long Next() {
      _s ^= _s << 13;
      _s ^= _s >> 7;
      _s ^= _s << 17;
      return (unsigned long)(_s >> 11) & 0x7FFFFFFFUL;
    }

    int Below(int n_) {
      return (int)(Next() % n_);
    }
};

/////////////////////////////////////////////////////////////////////////////
// the model

struct Row {
    int _i;
    std::string _s;
    std::string _b;
    t4_i64 _l;

    bool operator == (const Row &o_)const {
      return _i == o_._i && _l == o_._l && _s == o_._s && _b == o_._b;
    }
};

typedef std::vector < Row > Model;

static const char *kStructure = "a[i:I,s:S,b:B,l:L]";

// properties are made when first needed, not while statics are initialized
static c4_IntProp &PropI() {
  static c4_IntProp p("i");
  return p;
}

static c4_StringProp &PropS() {
  static c4_StringProp p("s");
  return p;
}

static c4_BytesProp &PropB() {
  static c4_BytesProp p("b");
  return p;
}

static c4_LongProp &PropL() {
  static c4_LongProp p("l");
  return p;
}

#define pi PropI()
#define ps PropS()
#define pb PropB()
#define pl PropL()

static std::string RandomBytes(Random &r_, int n_) {
  std::string s(n_, 0);
  // some runs of the same value, which is what is usual, and some noise
  int v = r_.Below(256);
  for (int k = 0; k < n_; ++k) {
    if (r_.Below(8) == 0)
      v = r_.Below(256);
    s[k] = (char)v;
  }
  return s;
}

static std::string RandomText(Random &r_) {
  int n = r_.Below(5) == 0 ? 0: r_.Below(40);
  std::string s;
  for (int k = 0; k < n; ++k)
    s += (char)('a' + r_.Below(26)); // no zero bytes in a string
  return s;
}

static int RandomBlobSize(Random &r_) {
  switch (r_.Below(10)) {
    case 0: return 0;
    case 1: case 2: case 3: return r_.Below(20);
    case 4: case 5: case 6: return r_.Below(600);
    case 7: case 8: return 3000 + r_.Below(6000);
    default: return 20000 + r_.Below(60000);
  }
}

static Row RandomRow(Random &r_) {
  Row row;
  switch (r_.Below(4)) {
    case 0: row._i = r_.Below(3); break;
    case 1: row._i = r_.Below(300) - 150; break;
    case 2: row._i = (int)r_.Next(); break;
    default: row._i =  - (int)r_.Next(); break;
  }
  row._l = ((t4_i64)r_.Next() << 31) ^ r_.Next();
  if (r_.Below(3) == 0)
    row._l = r_.Below(100);
  row._s = RandomText(r_);
  row._b = RandomBytes(r_, RandomBlobSize(r_));
  return row;
}

static void SetRow(c4_RowRef r_, const Row &row_) {
  pi(r_) = row_._i;
  ps(r_) = row_._s.c_str();
  pl(r_) = row_._l;
  pb(r_).SetData(c4_Bytes(row_._b.data(), (int)row_._b.size()));
}

static bool SameRow(c4_RowRef r_, const Row &row_) {
  if ((int)pi(r_) != row_._i || (t4_i64)pl(r_) != row_._l)
    return false;
  if (row_._s != (const char*)ps(r_))
    return false;
  c4_Bytes b;
  pb(r_).GetData(b);
  return b.Size() == (int)row_._b.size() && (b.Size() == 0 || memcmp
    (b.Contents(), row_._b.data(), b.Size()) == 0);
}

static bool Same(c4_View v_, const Model &m_) {
  if (v_.GetSize() != (int)m_.size())
    return false;
  for (int i = 0; i < v_.GetSize(); ++i)
    if (!SameRow(v_[i], m_[i]))
      return false;
  return true;
}

// the contents of a datafile, as it is persisted, opened for reading only
static bool SameInFile(const char *fn_, const Model &m_, int *rows_ = 0) {
  c4_Storage s(fn_, 0);
  if (!s.Strategy().IsValid())
    return false;
  c4_View v = s.GetAs(kStructure);
  if (rows_ != 0)
    *rows_ = v.GetSize();
  return Same(v, m_);
}

/////////////////////////////////////////////////////////////////////////////

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

static void WriteFile(const char *fn_, const std::string &data_, size_t n_) {
  FILE *f = fopen(fn_, "wb");
  CHECK(f != 0);
  CHECK(fwrite(data_.data(), 1, n_, f) == n_);
  fclose(f);
}

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

/////////////////////////////////////////////////////////////////////////////

// a commit which has been made, and where the datafile ended after it
struct Commit {
    Model _model;
    t4_fpos _end;
};

class Scenario {
  public:
    Scenario(unsigned long seed_): _r(seed_), _seed(seed_), _store(0),
      _junk(false) {}
    ~Scenario() {
      delete _store;
    }

    void Run();

  private:
    void Open();
    void Close();
    void Mutate();
    void DoCommit();
    void DoRollback();
    void DoReopen();
    void DoFailedCommit();
    void DoCrash();
    void DoTruncation();
    void SetMarks();
    t4_fpos FileSize();

    Random _r;
    unsigned long _seed;
    c4_Storage *_store;
    c4_View _view;
    std::string _fn;
    int _mode;
    int _marks; // 0: standard, 1: wide, 2: switch to wide at some point
    bool _wideNow;
    bool _junk; // the datafile has the remains of an interrupted commit

    Model _cur; // what the view should have now
    Model _committed; // what the datafile should hold, same as _hist.back()
    std::vector < Commit > _hist;
};

t4_fpos Scenario::FileSize() {
  return _store->Strategy().FileSize();
}

void Scenario::SetMarks() {
  f4_WideAbove = _wideNow ? 0 : kStdLimit;
}

void Scenario::Open() {
  _store = new c4_Storage(_fn.c_str(), _mode);
  _view = _store->GetAs(kStructure);
}

void Scenario::Close() {
  _view = c4_View();
  delete _store;
  _store = 0;
}

void Scenario::Mutate() {
  int n = (int)_cur.size();
  int op = _r.Below(100);

  if (op < 30 || n == 0) {
    Row row = RandomRow(_r);
    int at = _view.Add(pi[0]);
    SetRow(_view[at], row);
    _cur.push_back(row);
    LOG("  add row %d (%d bytes)\n", at, (int)row._b.size());
  } else if (op < 42) {
    Row row = RandomRow(_r);
    int at = _r.Below(n + 1);
    c4_Row tmp;
    SetRow(tmp, row);
    _view.InsertAt(at, tmp);
    _cur.insert(_cur.begin() + at, row);
    LOG("  insert at %d\n", at);
  } else if (op < 56) {
    int at = _r.Below(n);
    int count = 1 + _r.Below(_r.Below(4) == 0 ? n - at: 3);
    if (count > n - at)
      count = n - at;
    _view.RemoveAt(at, count);
    _cur.erase(_cur.begin() + at, _cur.begin() + at + count);
    LOG("  remove %d at %d\n", count, at);
  } else if (op < 66) {
    int at = _r.Below(n);
    _cur[at]._i = (int)_r.Next() - (int)_r.Next();
    pi(_view[at]) = _cur[at]._i;
    LOG("  int of row %d\n", at);
  } else if (op < 74) {
    int at = _r.Below(n);
    _cur[at]._s = RandomText(_r);
    ps(_view[at]) = _cur[at]._s.c_str();
    LOG("  string of row %d\n", at);
  } else if (op < 80) {
    int at = _r.Below(n);
    _cur[at]._l = ((t4_i64)_r.Next() << 32) ^ _r.Next();
    pl(_view[at]) = _cur[at]._l;
    LOG("  long of row %d\n", at);
  } else if (op < 90) {
    int at = _r.Below(n);
    _cur[at]._b = RandomBytes(_r, RandomBlobSize(_r));
    pb(_view[at]).SetData(c4_Bytes(_cur[at]._b.data(), (int)_cur[at]._b.size()));
    LOG("  bytes of row %d (%d)\n", at, (int)_cur[at]._b.size());
  } else {
    // change part of the bytes, in place, and maybe make them longer
    int at = _r.Below(n);
    std::string &b = _cur[at]._b;
    int off = b.empty() ? 0: _r.Below((int)b.size() + 1);
    std::string chunk = RandomBytes(_r, 1 + _r.Below(3000));
    CHECK(pb(_view[at]).Modify(c4_Bytes(chunk.data(), (int)chunk.size()), off));
    if (off + chunk.size() > b.size())
      b.resize(off + chunk.size());
    b.replace(off, chunk.size(), chunk);
    LOG("  modify bytes of row %d at %d (%d)\n", at, off, (int)chunk.size());
  }

  CHECK(Same(_view, _cur));
}

void Scenario::DoCommit() {
  SetMarks();
  LOG("  commit\n");
  CHECK(_store->Commit());

  // the datafile, as persisted, has what the model says
  CHECK(SameInFile(_fn.c_str(), _cur));

  _committed = _cur;
  Commit c;
  c._model = _cur;
  c._end = FileSize();
  if (_mode == 1 || c._end != _hist.back()._end)
    _hist.push_back(c);
  else
    _hist.back()._model = _cur;
}

void Scenario::DoRollback() {
  LOG("  rollback\n");
  CHECK(_store->Rollback());
  _view = _store->GetAs(kStructure);
  _cur = _committed;
  CHECK(Same(_view, _cur));
  CHECK(SameInFile(_fn.c_str(), _committed));
}

void Scenario::DoReopen() {
  LOG("  reopen, dropping what was not committed\n");
  Close();
  Open();
  _cur = _committed;
  CHECK(Same(_view, _cur));
}

// a commit which fails must leave the datafile and the view as they were
void Scenario::DoFailedCommit() {
  LOG("  failing commit\n");
  std::string before;
  CHECK(ReadFile(_fn.c_str(), before));

  SetMarks();

  // the engine counts from the start of the datafile proper, which is not
  // at the start of the file if what came before it was not a datafile
  const t4_fpos size = FileSize() - _store->Strategy()._baseOffset;
  f4_MaxFileEnd = size > 0 ? size - 1: 0;
  CHECK(!_store->Commit());
  CHECK(!_store->Commit());
  f4_MaxFileEnd = kMaxEndDefault;

  std::string after;
  CHECK(ReadFile(_fn.c_str(), after));
  CHECK(before == after);
  CHECK(Same(_view, _cur));
  CHECK(SameInFile(_fn.c_str(), _committed));
}

// a commit which stops after some number of writes: the datafile must have
// the old contents or the new ones, nothing else
void Scenario::DoCrash() {
  int budget;
  switch (_r.Below(4)) {
    case 0: case 1: budget = _r.Below(40); break;
    case 2: budget = 40 + _r.Below(400); break;
    default: budget = 1000000; // not interrupted at all
  }
  LOG("  crash after %d writes\n", budget);

  // the changes are made again in a new session, with the strategy which
  // is going to stop, so first the changes have to be there to make
  const Model wanted = _cur;
  Close();
  {
    CrashStrategy cs(budget);
    cs.DataOpen(_fn.c_str(), 1);
    c4_Storage s(cs, false, _mode);
    c4_View v = s.GetAs(kStructure);
    CHECK(Same(v, _committed));
    v.SetSize(0);
    for (size_t k = 0; k < wanted.size(); ++k) {
      int at = v.Add(pi[0]);
      SetRow(v[at], wanted[k]);
    }
    SetMarks();
    s.Commit();
  }

  int rows = 0;
  const bool isNew = SameInFile(_fn.c_str(), wanted, &rows);
  const bool isOld = SameInFile(_fn.c_str(), _committed, &rows);
  if (!isNew && !isOld) {
    int fileRows = 0;
    SameInFile(_fn.c_str(), wanted, &fileRows);
    fprintf(stderr, "seed %lu: after crash (budget %d, mode %d): file has %d "
      "rows, old state %d rows, new state %d rows\n", _seed, budget, _mode,
      fileRows, (int)_committed.size(), (int)wanted.size());
  }
  if (!isNew && !isOld && getenv("TFUZZ_KEEP") != 0) {
    std::string data;
    char keep[64];
    sprintf(keep, "tfuzz_%lu_failed.mk", _seed);
    if (ReadFile(_fn.c_str(), data))
      WriteFile(keep, data, data.size());
  }
  CHECK(isNew || isOld);

  if (isNew && !(wanted == _committed)) {
    _committed = wanted;
    Commit c;
    c._model = wanted;
    c._end = 0;
    {
      c4_Storage s(_fn.c_str(), 0);
      c._end = s.Strategy().FileSize();
    }
    _hist.push_back(c);
  } else if (!isNew || budget < 1000000) {
    _junk = true; // there may be remains of the commit at the end
  }
  _cur = _committed;
  Open();
  CHECK(Same(_view, _cur));
}

// the datafile cut short, as by a crash, or a copy which stopped: the result
// has to be the empty database or a commit, see twide.cpp for the details
void Scenario::DoTruncation() {
  std::string whole;
  CHECK(ReadFile(_fn.c_str(), whole));
  if (whole.empty())
    return ;

  // preferably near where a commit ended, or at the end
  size_t L;
  switch (_r.Below(4)) {
    case 0: L = _r.Below((int)whole.size() + 1); break;
    case 1: {
      t4_fpos e = _hist[_r.Below((int)_hist.size())]._end;
      L = (size_t)e + _r.Below(40) - 20;
      break;
    }
    case 2: L = whole.size() - _r.Below(40); break;
    default: L = whole.size() - _r.Below(2000);
  }
  if (L > whole.size())
    L = whole.size();
  LOG("  cut at %d of %d\n", (int)L, (int)whole.size());

  // In the default mode, a datafile cut exactly where an older commit ended
  // looks like a complete one, but later commits have overwritten parts of it.
  // The engine trusts what it loads, and what is in the file is not a datafile
  // any more: it may well be that it cannot be opened without crashing.  This
  // is not tried, see twide.cpp, nor is the engine hardened against that.
  for (size_t k = 0; k + 1 < _hist.size(); ++k)
    if (_mode == 1 && _hist[k]._end == (t4_fpos)L && L > 0 && _hist.back().
      _end != (t4_fpos)L) {
      LOG("  (cut at the end of an older commit is not tried)\n");
      return ;
    }

  const char *cut = "tfuzz_cut.mk";
  WriteFile(cut, whole, L);
  if (getenv("TFUZZ_KEEP") != 0) {
    char keep[64];
    sprintf(keep, "tfuzz_%lu_cut%d.mk", _seed, sStep);
    WriteFile(keep, whole, L);
  }

  int rows = 0;
  bool found = false;
  int best = -1; // the last commit which was entirely there
  for (size_t k = 0; k < _hist.size(); ++k) {
    if (_hist[k]._end <= (t4_fpos)L && _hist[k]._end > 0)
      best = (int)k;
    if (!found && SameInFile(cut, _hist[k]._model, &rows) && !(k == 0 &&
      rows != 0))
      found = true;
  }
  bool empty = false;
  {
    c4_Storage s(cut, 0);
    empty = s.Strategy().IsValid() && s.GetAs(kStructure).GetSize() == 0;
  }

  bool oldEnd = false; // cut exactly where an older commit ended
  for (size_t k = 0; k + 1 < _hist.size(); ++k)
    if (_hist[k]._end == (t4_fpos)L)
      oldEnd = true;

  if (_mode == 2 && !_junk && best >= 0) {
    // everything is still there up to that commit, so it is that one
    CHECK(SameInFile(cut, _hist[best]._model));
  } else if (_mode == 2 && best >= 0) {
    // with the remains of an interrupted commit in the middle, the commits
    // after it cannot be reached from the header, so one of the earlier ones
    CHECK(found || empty);
  } else {
    // the default mode keeps only the last one valid, see twide.cpp
    CHECK(found || empty || oldEnd);
  }

  // and a datafile like that must be possible to change
  {
    Model m;
    c4_Storage s(cut, _mode);
    c4_View v = s.GetAs(kStructure);
    for (int k = 0; k < v.GetSize(); ++k) {
      Row row;
      row._i = pi(v[k]);
      row._l = pl(v[k]);
      row._s = (const char*)ps(v[k]);
      c4_Bytes b;
      pb(v[k]).GetData(b);
      row._b.assign((const char*)b.Contents(), b.Size());
      m.push_back(row);
    }
    // what was there is not touched by adding to it, as a model is made of it
    Row extra = RandomRow(_r);
    int at = v.Add(pi[0]);
    SetRow(v[at], extra);
    m.push_back(extra);
    SetMarks();
    const bool ok = s.Commit();
    LOG("  (opened with %d rows, commit %s, mode %d, base %lld, file %lld)\n",
      (int)m.size() - 1, ok ? "ok": "FAILED", _mode, (long long)
      s.Strategy()._baseOffset, (long long)s.Strategy().FileSize());
    CHECK(ok);
    int rows = 0;
    const bool same = SameInFile(cut, m, &rows);
    if (!same)
      fprintf(stderr, "seed %lu: after changing the cut datafile it has %d "
        "rows, expected %d\n", _seed, rows, (int)m.size());
    CHECK(same);
  }
  remove(cut);
}

void Scenario::Run() {
  sSeed = _seed;
  _mode = 1 + _r.Below(2);
  _marks = _r.Below(3);
  _wideNow = _marks == 1;
  char name[64];
  sprintf(name, "tfuzz_%lu.mk", _seed);
  _fn = name;
  remove(name);

  LOG("seed %lu: mode %d, marks %s\n", _seed, _mode, _marks == 0 ? "standard":
    _marks == 1 ? "wide": "standard, then wide");

  Open();

  Commit first;
  first._end = 0;
  _hist.push_back(first); // the empty database, before anything was committed

  const int steps = 20 + _r.Below(60);
  for (sStep = 0; sStep < steps; ++sStep) {
    if (_marks == 2 && !_wideNow && _r.Below(12) == 0)
      _wideNow = true;

    const bool pending = !(_cur == _committed);
    int op = _r.Below(100);
    if (op < 52)
      Mutate();
    else if (op < 68)
      DoCommit();
    else if (op < 72)
      DoRollback();
    else if (op < 77)
      DoReopen();
    else if (op < 84 && pending && _committed.size() + _hist.size() > 1)
      DoFailedCommit();
    else if (op < 92 && pending)
      DoCrash();
    else if (op < 97 && _hist.size() > 1)
      DoTruncation();
    else
      Mutate();

    if (getenv("TFUZZ_SNAP") != 0) {
      std::string data;
      char snap[64];
      sprintf(snap, "tfuzz_%lu_step%02d.mk", _seed, sStep);
      if (ReadFile(_fn.c_str(), data))
        WriteFile(snap, data, data.size());
    }
  }

  // whatever happened, a last commit puts everything where it can be checked
  DoCommit();
  Close();
  CHECK(SameInFile(_fn.c_str(), _cur));
  remove(_fn.c_str());
}

/////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv) {
  unsigned long first = 1, count = getenv("TFUZZ_LONG") != 0 ? 1000: 40;

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

  // Seeds which showed problems in the engine, and are always tried. They
  // were found with the first versions of this program, and mean what they did
  // as long as the scenarios are the same, so they are of no use if the random
  // choices in here change: then new ones have to be found.
  //   4, 16, 28, 2599, 5604, 17840, 35547: interrupted commits, more than one
  //   in a datafile, which were not recovered or lost commits before them
  static const unsigned long kKnown[] =  {
    4, 16, 28, 2599, 5604, 17840, 35547
  };

  int failed = 0;
  std::vector < unsigned long > seeds;
  for (unsigned long seed = first; seed < first + count; ++seed)
    seeds.push_back(seed);
  if (a >= argc && !sVerbose) // no seeds were asked for
    for (int k = 0; k < (int)(sizeof kKnown / sizeof *kKnown); ++k)
      seeds.push_back(kKnown[k]);

  for (size_t n = 0; n < seeds.size(); ++n) {
    const unsigned long seed = seeds[n];
    f4_WideAbove = kStdLimit;
    f4_MaxFileEnd = kMaxEndDefault;
    try {
      Scenario s(seed);
      s.Run();
    } catch (int) {
      ++failed;
      fprintf(stderr, "  repeat with: tfuzz -v %lu\n", seed);
    }
  }
  f4_WideAbove = kStdLimit;
  f4_MaxFileEnd = kMaxEndDefault;

  printf("%d scenarios, %d failed\n", (int)seeds.size(), failed);
  return failed == 0 ? 0 : 1;
}
