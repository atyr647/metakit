// tcrash.cpp -- Test program for what a power cut leaves behind of a commit
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

// A process crash loses nothing which was written, but a power cut can lose
// any write which was not forced to disk (c4_Strategy::DataSync), and the
// writes which are still on their way to the disk reach it in any order, or
// only in part (a sector at a time).  This program runs commits on a model of
// that: every write goes to a list of "pending" writes, and a call of DataSync
// makes everything written so far permanent.  After each write of a commit,
// every combination of the pending writes is taken to be what the disk was
// left with (all of them when there are few, else a selection), and read
// back.  The result must be the state before the commit or the one after it,
// and never damaged.  Some of these disks are then given another commit.
//
// The model only knows what DataSync promises.  That the real strategy forces
// the data to disk when it is called, and in which order it does so, is
// checked by tests/tsync.sh.
//
// This program is built and run by "make test" on Unix, next to regress.
// With an argument it also shows that the model finds the problem when
// commits do not force data to disk, or only at one of the two places.

#include "mk4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the hook which sets the size above which wide marks are used
extern t4_fpos f4_WideAbove;
static const t4_fpos kStdLimit = 0x7fffffff - 64; // the default

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(c) do { ++sChecks; if (!(c)) { \
    fprintf(stderr, "CHECK FAILED line %d: %s\n", __LINE__, #c); \
    ++sFailures; } } while (0)

/////////////////////////////////////////////////////////////////////////////
// The model of a file on a disk which can lose what was not forced to it

enum {
  kSector = 512 // writes reach the disk in parts of this size, in any order
};

// memcpy which does not mind being asked for nothing, from nowhere
static void Copy(void *to_, const void *from_, size_t n_) {
  if (n_ > 0)
    memcpy(to_, from_, n_);
}

struct Piece {
    t4_fpos _pos;
    int _len;
    t4_byte _data[kSector];
};

struct Image {
    t4_byte *_p;
    t4_fpos _size, _cap;

    Image(): _p(0), _size(0), _cap(0){}
    ~Image() {
      free(_p);
    }

    void Put(t4_fpos pos_, const t4_byte *data_, int len_) {
      if (pos_ + len_ > _cap) {
        t4_fpos cap = (pos_ + len_) * 2 + 1024;
        _p = (t4_byte*)realloc(_p, (size_t)cap);
        memset(_p + _cap, 0, (size_t)(cap - _cap));
        _cap = cap;
      }
      Copy(_p + pos_, data_, len_);
      if (pos_ + len_ > _size)
        _size = pos_ + len_;
    }

    void Assign(const Image &other_) {
      _size = 0;
      if (_cap < other_._size) {
        free(_p);
        _cap = other_._size + 1024;
        _p = (t4_byte*)calloc(1, (size_t)_cap);
      } else
        if (_cap > 0)
          memset(_p, 0, (size_t)_cap);
      if (other_._size > 0)
        Put(0, other_._p, (int)other_._size);
      _size = other_._size;
    }
};

struct Event {
    bool _write; // a piece of a write, else a call of DataSync
    int _call; // the write which it is a piece of
    bool _barrier; // for DataSync: it is forced to disk (see Resolve)
    Piece _piece;
};

class PowerFile: public c4_Strategy {
  public:
    Image _cur; // what the program sees
    Image _dur; // what is on the disk for sure
    Piece *_pend; // what is not yet there
    int _npend;
    Event *_ev; // the writes and calls of DataSync of the commit being done
    int _nev, _evCap;
    bool _recording;
    int _calls; // number of calls of DataWrite so far

    PowerFile(): _pend(0), _npend(0), _ev(0), _nev(0), _evCap(0),
      _recording(false), _calls(0){}

    ~PowerFile() {
      free(_pend);
      free(_ev);
    }

    virtual bool IsValid()const {
      return true;
    }

    virtual t4_fpos FileSize() {
      return _cur._size;
    }

    virtual int DataRead(t4_fpos pos_, void *buf_, int len_) {
      if (pos_ < 0)
        return  - 1;
      if (pos_ + len_ > _cur._size)
        len_ = pos_ >= _cur._size ? 0: (int)(_cur._size - pos_);
      Copy(buf_, _cur._p + pos_, len_);
      return len_;
    }

    virtual void DataWrite(t4_fpos pos_, const void *buf_, int len_) {
      _cur.Put(pos_, (const t4_byte*)buf_, len_);
      ++_calls;

      // cut at the sector boundaries, each part can be there or not
      for (int done = 0; done < len_;) {
        const t4_fpos at = pos_ + done;
        int n = kSector - (int)(at % kSector);
        if (n > len_ - done)
          n = len_ - done;

        _pend = (Piece*)realloc(_pend, (_npend + 1) * sizeof(Piece));
        Piece &pc = _pend[_npend++];
        pc._pos = at;
        pc._len = n;
        memcpy(pc._data, (const t4_byte*)buf_ + done, n);

        if (_recording) {
          if (_nev == _evCap) {
            _evCap = _evCap * 2 + 64;
            _ev = (Event*)realloc(_ev, _evCap * sizeof(Event));
          }
          _ev[_nev]._write = true;
          _ev[_nev]._call = _calls;
          _ev[_nev]._barrier = false;
          _ev[_nev]._piece = pc;
          ++_nev;
        }
        done += n;
      }
    }

    // flushing is not forcing to disk: that is DataSync
    virtual void DataCommit(t4_fpos){}

    // whether this does what it promises is settled after the commit
    virtual void DataSync() {
      if (!_recording) {
        _dur.Assign(_cur);
        _npend = 0;
        return ;
      }
      if (_nev == _evCap) {
        _evCap = _evCap * 2 + 64;
        _ev = (Event*)realloc(_ev, _evCap * sizeof(Event));
      }
      _ev[_nev]._write = false;
      _ev[_nev]._barrier = false;
      ++_nev;
    }
};

// a datafile as the disk was left with it, to be read and nothing else
class DiskImage: public c4_Strategy {
  public:
    const Image &_img;

    DiskImage(const Image &img_): _img(img_){}

    virtual bool IsValid()const {
      return true;
    }

    virtual t4_fpos FileSize() {
      return _img._size;
    }

    virtual int DataRead(t4_fpos pos_, void *buf_, int len_) {
      if (pos_ < 0)
        return  - 1;
      if (pos_ + len_ > _img._size)
        len_ = pos_ >= _img._size ? 0: (int)(_img._size - pos_);
      Copy(buf_, _img._p + pos_, len_);
      return len_;
    }
};

/////////////////////////////////////////////////////////////////////////////
// A workload, and a way to boil down what is in a datafile to a number

static unsigned long Signature(c4_View v_) {
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
    for (int k = 0; k < b.Size(); k += 7)
      h = h * 31 + b.Contents()[k];
    h += b.Size();
  }
  return h;
}

// Moves the end of the file, and with it the place of the commit tail: for
// some values it is across a boundary between sectors.
static int sPad = 0;

// each step is one commit, they include changes which fit in the space of
// the file (which are made in place) and ones which have to extend it
static void Step(c4_View v_, int n_) {
  c4_IntProp pi("i");
  c4_StringProp ps("s");
  c4_BytesProp pb("b");

  switch (n_) {
    case 0:
      for (int i = 0; i < 12; ++i) {
        char tmp[1400];
        sprintf(tmp, "first-%d-%*d", i, i * 3 + (i == 0 ? sPad: 0), i);
        v_.Add(pi[i] + ps[tmp]);
      }
      break;
    case 1:
      {
        int r = v_.Add(pi[100] + ps["with a memo"]);
        t4_byte b[1700];
        for (int k = 0; k < (int)sizeof b; ++k)
          b[k] = (t4_byte)(k * 7 + 1);
        pb(v_[r]).SetData(c4_Bytes(b, sizeof b));
      }
      break;
    case 2:
      pi(v_[2]) = 4242;
      break;
    case 3:
      v_.RemoveAt(3, 6);
      break;
    case 4:
      pi(v_[1]) = 99;
      break;
    case 5:
      v_.Add(pi[500] + ps["after the removal"]);
      break;
    case 6:
      pi(v_[0]) = 12345;
      break;
    case 7:
      pi(v_[0]) = 54321;
      break;
    default:
      pi(v_[0]) = 7 + n_;
      break;
  }
}

static const int kSteps = 9;

/////////////////////////////////////////////////////////////////////////////
// Crashes

static unsigned sSeed = 12345;
static unsigned Rand() {
  sSeed = sSeed * 1103515245u + 12345u;
  return (sSeed >> 8) &0xFFFFFF;
}

struct Totals {
    long _images, _bad, _damaged;
};

// the state of the disk after the first "upto" events, with the mask of
// pending writes which got there
static void Apply(Image &img_, const Piece &pc_) {
  img_.Put(pc_._pos, pc_._data, pc_._len);
}

// read the datafile in an image, and say whether it is one of the two states
// which are acceptable (0 stands for no data at all)
static bool Acceptable(const Image &img_, unsigned long before_, unsigned long
  after_, bool *damaged_) {
  DiskImage di(img_);
  c4_Storage s(di, false, 0);
  c4_View v = s.View("a");
  *damaged_ = s.IsDamaged();
  const unsigned long sig = Signature(v);
  return sig == before_ || sig == after_;
}

// whether disks are given more commits: only for those which are meant to work
static bool sGoOn = true;

// a datafile which was left by a crash must take more commits
static void GoOn(const Image &img_, int mode_) {
  PowerFile pf;
  pf._cur.Assign(img_);
  pf._dur.Assign(img_);

  c4_IntProp pi("i");
  c4_StringProp ps("s");
  unsigned long expect = 0;
  {
    c4_Storage s(pf, false, mode_);
    c4_View v = s.GetAs("a[i:I,s:S,b:B]");
    for (int round = 0; round < 3; ++round) {
      if (v.GetSize() > 0)
        pi(v[0]) = 31337 + round;
      v.Add(pi[777 + round] + ps["added after the crash"]);
      CHECK(s.Commit());
    }
    expect = Signature(v);
  }

  DiskImage di(pf._cur);
  c4_Storage s(di, false, 0);
  c4_View v = s.View("a");
  CHECK(Signature(v) == expect);
  CHECK(!s.IsDamaged());
}

static void TryImages(const Image &dur_, const Piece *pend_, int npend_,
  unsigned long before_, unsigned long after_, Totals &t_, int round_,
  int mode_) {
  Image img;
  // the combinations are bits of a 64-bit number (when it is more than that,
  // as it is if nothing is ever forced to disk, only the first are in them)
  const int n = npend_ < 63 ? npend_: 63;
  if (sGoOn)
    CHECK(npend_ < 64);
  const bool all = n <= 8;
  const int tries = all ? 1 << n: 2 * n + 400;

  for (int k = 0; k < tries; ++k) {
    typedef unsigned long long u64;
    u64 mask;
    if (all)
      mask = k;
    else if (k < n)
      mask = (((u64)1) << (k + 1)) - 1; // the first writes
    else if (k < 2 * n)
      mask = ~((((u64)1) << (k - n)) - 1); // the last writes
    else
      mask = ((u64)Rand() << 24) ^ (u64)Rand() ^ ((u64)Rand() << 48);

    img.Assign(dur_);
    for (int i = 0; i < n && i < 64; ++i)
      if (mask >> i &1)
        Apply(img, pend_[i]);

    bool damaged = false;
    const bool ok = Acceptable(img, before_, after_, &damaged);
    if (sGoOn && ok && !damaged && k % 41 == 0)
      GoOn(img, mode_);
    ++t_._images;
    if (!ok)
      ++t_._bad;
    if (damaged)
      ++t_._damaged;
    if ((!ok || damaged) && t_._bad + t_._damaged < 4)
      fprintf(stderr, "  not acceptable: mode %d, step %d, %d pending, mask %llx"
        "%s\n", mode_, round_, n, mask, damaged ? " (found damaged)": "");
  }
}

// The disk as it is at some point: what is there for sure, and what is on
// its way (which is there or not, in any combination)
struct Disk {
    Image _dur;
    Piece *_pend;
    int _npend;

    Disk(): _pend(0), _npend(0){}
    ~Disk() {
      free(_pend);
    }

    void Start(const Image &dur_, const Piece *pend_, int npend_, int room_) {
      _dur.Assign(dur_);
      free(_pend);
      _pend = (Piece*)malloc((npend_ + room_ + 1) * sizeof(Piece));
      Copy(_pend, pend_, npend_ * sizeof(Piece));
      _npend = npend_;
    }

    void Happen(const Event &ev_) {
      if (ev_._write)
        _pend[_npend++] = ev_._piece;
      else if (ev_._barrier) {
        for (int i = 0; i < _npend; ++i)
          Apply(_dur, _pend[i]);
        _npend = 0;
      }
    }
};

// Say which calls of DataSync in the commit just made are forced to disk: bit
// 0 of the policy is for the first call, bit 1 for the second.  A commit makes
// two: one before the commit tail is written, which has to make everything
// which the tail leads to permanent, and one after it.
enum {
  kBeforeTail = 1, kAfterTail = 2, kBoth = 3
};

static void Resolve(PowerFile &pf_, unsigned policy_) {
  int syncs = 0;
  for (int e = 0; e < pf_._nev; ++e)
    if (!pf_._ev[e]._write) {
      ++syncs;
      pf_._ev[e]._barrier = (policy_ >> (syncs - 1) &1) != 0;
    }
  CHECK(syncs == 2);
}

// try the crash after each event of a commit
static void CrashDuring(const PowerFile &pf_, const Image &dur0_, const Piece
  *pend0_, int npend0_, unsigned long before_, unsigned long after_, Totals
  &t_, int round_, int mode_) {
  Disk d;
  d.Start(dur0_, pend0_, npend0_, pf_._nev);
  TryImages(d._dur, d._pend, d._npend, before_, after_, t_, round_, mode_);
  for (int e = 0; e < pf_._nev; ++e) {
    d.Happen(pf_._ev[e]);
    TryImages(d._dur, d._pend, d._npend, before_, after_, t_, round_, mode_);
  }
}

// run the workload, a commit at a time, with every crash tried along the way
static Totals Run(int mode_, bool wide_, unsigned policy_, int pad_ = 0) {
  Totals t = {0, 0, 0};
  f4_WideAbove = wide_ ? 0 : kStdLimit;
  sPad = pad_;

  PowerFile pf;
  c4_Storage *s = new c4_Storage(pf, false, mode_);
  c4_View v = s->GetAs("a[i:I,s:S,b:B]");
  unsigned long before = 0;

  for (int step = 0; step < kSteps; ++step) {
    Step(v, step);
    const unsigned long after = Signature(v);

    Disk d0;
    d0.Start(pf._dur, pf._pend, pf._npend, 0);

    pf._nev = 0;
    pf._recording = true;
    const bool ok = s->Commit();
    pf._recording = false;
    CHECK(ok);
    Resolve(pf, policy_);

    CrashDuring(pf, d0._dur, d0._pend, d0._npend, step == 0 ? 0: before, after,
      t, step, mode_);

    // what the disk has now, when the commit is done
    Disk d1;
    d1.Start(d0._dur, d0._pend, d0._npend, pf._nev);
    for (int e = 0; e < pf._nev; ++e)
      d1.Happen(pf._ev[e]);
    pf._dur.Assign(d1._dur);
    free(pf._pend);
    pf._pend = (Piece*)malloc((d1._npend + 1) * sizeof(Piece));
    Copy(pf._pend, d1._pend, d1._npend * sizeof(Piece));
    pf._npend = d1._npend;

    // a program which was started again, now and then
    if (step % 3 == 2) {
      v = c4_View();
      delete s;
      s = new c4_Storage(pf, false, mode_);
      v = s->View("a");
    }
    before = after;
  }

  v = c4_View();
  delete s;
  f4_WideAbove = kStdLimit;
  return t;
}

// A datafile which is added behind something else, in a strategy which does not
// memory-map (as is the case without mmap, and for a datafile too large for
// the address space), has a base offset and no mapped data.  A column was
// then taken to be in the mapped file, whatever its buffer.  This is not
// seen without a sanitizer, see tests/sanitize.sh.
static void TestBehindOther() {
  printf("a datafile behind other data, without a memory map\n");

  PowerFile pf;
  t4_byte other[5000];
  memset(other, 'x', sizeof other);
  pf._cur.Put(0, other, sizeof other);
  pf._dur.Put(0, other, sizeof other);

  c4_IntProp pi("i");
  {
    c4_Storage s(pf, false, 1);
    c4_View v = s.GetAs("a[i:I]");
    for (int i = 0; i < 3000; ++i)
      v.Add(pi[i]);
    CHECK(s.Commit());
    v.Add(pi[1]);
    pi(v[5]) = 77;
    CHECK(s.Commit());
  }

  DiskImage di(pf._cur);
  c4_Storage s(di, false, 0);
  c4_View v = s.View("a");
  CHECK(v.GetSize() == 3001);
  CHECK(v.GetSize() > 5 && pi(v[5]) == 77);
  CHECK(v.GetSize() > 3000 && pi(v[3000]) == 1);
}

int main(int argc, char **argv) {
  const bool explore = argc > 1;

  TestBehindOther();

  printf("power cut model, data forced to disk before and after the commit "
    "tail\n");
  for (int wide = 0; wide < 2; ++wide)
    for (int mode = 1; mode <= 2; ++mode) {
      const Totals t = Run(mode, wide != 0, kBoth);
      printf("  mode %d, %s marks: %ld disks tried, %ld not acceptable, %ld "
        "damaged\n", mode, wide ? "wide": "standard", t._images, t._bad,
        t._damaged);
      CHECK(t._images > 1000);
      CHECK(t._bad == 0 && t._damaged == 0);
    }

  // with these the commit tail of a commit made in place is across a boundary
  // between two sectors, which a disk may write one after the other
  static const int pads[] =  {
    21, 22, 264, 265, 520, 521, 776, 777, 1032, 1033
  };
  printf("power cut model, commit tails at the edge of a sector\n");
  long tried = 0, bad = 0;
  for (int i = 0; i < (int)(sizeof pads / sizeof *pads); ++i) {
    const Totals t = Run(1, false, kBoth, pads[i]);
    tried += t._images;
    bad += t._bad + t._damaged;
  }
  printf("  mode 1: %ld disks tried, %ld not acceptable or damaged\n", tried,
    bad);
  CHECK(bad == 0);

  if (explore) {
    sGoOn = false;
    printf("power cut model, data not forced to disk (bad results are what is "
      "expected)\n");
    for (int mode = 1; mode <= 2; ++mode)
      for (unsigned p = 0; p < 3; ++p) {
        const Totals t = Run(mode, false, p);
        printf("  mode %d, forced %s: %ld disks tried, %ld not acceptable, "
          "%ld damaged\n", mode, p == 0 ? "never": p == kBeforeTail ?
          "before the tail only": "after the tail only", t._images, t._bad,
          t._damaged);
        CHECK(t._bad > 0);
      }
  }

  printf("%d checks, %s\n", sChecks, sFailures == 0 ? "PASSED" : "FAILED");
  return sFailures == 0 ? 0 : 1;
}
