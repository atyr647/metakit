// marks.h --
// $Id$
// This is part of Metakit, see http://www.equi4.com/metakit.html

/** @file
 * Definition of the "marks" which delimit a datafile and its commits.
 */

#ifndef __MARKS_H__
#define __MARKS_H__

/////////////////////////////////////////////////////////////////////////////
//
//  A datafile starts with an 8-byte header and ends with "tails".  Each
//  commit appends a skip tail and a commit tail (in that order).  Readers
//  scan backwards from the end of the file: the commit tail says where the
//  root of the data is, the skip tail leads back to the header.  All values
//  are stored in big-endian order, the root length is always a 24-bit value.
//
//  Standard marks, used as long as the file stays below 2 Gb:
//
//    header         'J' 'L' 1A  00   <offset: 32 bits>   (80 for old files)
//    skip tail       80  <0: 24 bits>  <skip: 32 bits>   16 bytes in all
//    commit tail     80  <root length: 24 bits>  <root position: 32 bits>
//
//  The skip value is the position of the skip tail itself.  The offset in
//  the header is the file size, but file readers do not depend on it.
//
//  Wide marks are used for files which need positions over 31 bits.  The
//  tails then take 24 bytes, in the order skip, extension and commit:
//
//    skip tail       82  00 00 00  <skip: low 32 bits>
//    extension       83  <skip: bits 32..55>  <root position: bits 32..63>
//    commit tail     81  <root length: 24 bits>  <root position: low 32 bits>
//
//  Old programs do not know types 81..83, so they refuse such a file, which
//  is better than reading it wrongly.  The header of a wide file has bit
//  0x40 set in the fourth byte (which old programs also refuse), and then
//  has 6 more bits of the offset in the remaining bits of that byte.  That
//  offset saturates, it is only used for loading from a stream.
//
//  Commits to a wide file never overwrite existing tails: the three tail
//  marks cannot be replaced in one atomic write as the commit tail of a
//  standard file can.  They are appended instead, like in "extend" mode.
//
/////////////////////////////////////////////////////////////////////////////

enum {
  kMarkTail = 0x80, // skip and commit tails of standard marks
  kMarkWideCommit = 0x81, // type of a wide commit tail
  kMarkWideSkip = 0x82, // type of a wide skip tail
  kMarkWideExt = 0x83, // type of the extension between those two
  kHeadWide = 0x40, // flag in the fourth header byte
  kStdTailSize = 16, // size of the standard skip and commit tails together
  kWideTailSize = 24 // size of the wide skip, extension and commit tails
};

// A wide datafile can have positions up to 2^56, beyond that it is refused
#if q4_FPOS64
#define f4_kMaxWideEnd (((t4_fpos)1 << 56) - 64)
#else
#define f4_kMaxWideEnd (0x7fffffff - 64)
#endif

// Datafiles up to this size (before adding the tails) use standard marks,
// and those over it use wide marks.  This is a variable only for testing.
extern t4_fpos f4_WideAbove;

/////////////////////////////////////////////////////////////////////////////

// store the low 32 bits of a value
inline void f4_Put32(t4_byte *p_, t4_fpos v_) {
  for (int i = 24; i >= 0; i -= 8)
    *p_++ = (t4_byte)(v_ >> i);
}

// fetch 32 bits as an unsigned value
inline t4_fpos f4_Get32(const t4_byte *p_) {
  t4_fpos v = 0;
  for (int i = 0; i < 4; ++i)
    v = (v << 8) + p_[i];
  return v;
}

// bits 32..63 of a value, which is zero if there are no more than 32 bits
inline t4_fpos f4_High32(t4_fpos v_) {
#if q4_FPOS64
  return (v_ >> 32) &0xFFFFFFFF;
#else
  return 0;
#endif
}

// combine the low and high 32 bits
inline t4_fpos f4_Join32(t4_fpos hi_, t4_fpos lo_) {
#if q4_FPOS64
  return (hi_ << 32) + lo_;
#else
  d4_assert(hi_ == 0);
  return lo_;
#endif
}

// Fill in a wide skip tail followed by an extension, 16 bytes.  The root
// position is not yet known at this point, and the commit will repeat the
// extension with it.
inline void f4_WideSkipMarks(t4_byte *b_, t4_fpos skipPos_) {
  b_[0] = kMarkWideSkip;
  b_[1] = b_[2] = b_[3] = 0;
  f4_Put32(b_ + 4, skipPos_);

  const t4_fpos skipHi = f4_High32(skipPos_);
  b_[8] = kMarkWideExt;
  b_[9] = (t4_byte)(skipHi >> 16);
  b_[10] = (t4_byte)(skipHi >> 8);
  b_[11] = (t4_byte)skipHi;
  f4_Put32(b_ + 12, 0);
}

// Fill in the extension and the commit tail, 16 bytes
inline void f4_WideCommitMarks(t4_byte *b_, t4_fpos skipPos_, t4_fpos rootPos_,
  int rootLen_) {
  b_[0] = kMarkWideExt;
  const t4_fpos skipHi = f4_High32(skipPos_);
  b_[1] = (t4_byte)(skipHi >> 16);
  b_[2] = (t4_byte)(skipHi >> 8);
  b_[3] = (t4_byte)skipHi;
  f4_Put32(b_ + 4, f4_High32(rootPos_));

  b_[8] = kMarkWideCommit;
  b_[9] = (t4_byte)(rootLen_ >> 16);
  b_[10] = (t4_byte)(rootLen_ >> 8);
  b_[11] = (t4_byte)rootLen_;
  f4_Put32(b_ + 12, rootPos_ &0xFFFFFFFF);
}

#endif
