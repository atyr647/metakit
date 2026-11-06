// column.h --
// $Id$
// This is part of Metakit, see http://www.equi4.com/metakit.html

/** @file
 * Definition of the column classes
 */

#ifndef __COLUMN_H__
#define __COLUMN_H__

/////////////////////////////////////////////////////////////////////////////
// Declarations in this file

class c4_Column; // a column in a table
class c4_ColIter; // an iterator over column data
class c4_ColCache; // manages a cache for columns

class c4_Persist; // not defined here
class c4_Strategy; // not defined here

/////////////////////////////////////////////////////////////////////////////

// The size of one column is a 32-bit value in the datafile, so it cannot
// exceed this, however large the datafile itself may be.  This applies to
// a single column, i.e. all the integers (or all the strings, or all the
// data bytes) of one property, as well as to every memo.  The engine checks
// it whenever a column grows: it is a variable only so tests can lower it.
extern t4_i32 f4_MaxColumnSize;

// A position in the buffers of a column, which hold its gap as well as its
// data: they go up to the size of the column plus the gap, which is less than
// two segments.  For a column near the limit that is more than 2^31, so these
// positions are 64-bit, while what the column holds is addressed with t4_i32.
// Where there are no 64-bit integers, the limit is lower instead (column.cpp).
#if q4_FPOS64
typedef t4_i64 t4_cpos;
#else 
typedef t4_i32 t4_cpos;
#endif 

// Reports a column which would become too large, see c4_SetSizeErrorHandler.
// This is called before anything is changed, and does not return.
extern void f4_ColumnTooLarge(t4_fpos wanted_);

// Everything in the structure of a datafile (what views there are, how many
// rows, where the columns are) is read from bytes which cannot be trusted:
// the datafile may be damaged.  While one is read, this holds where it ends, so
// that what is read can not go beyond that, and whether something was wrong
// with it, which is for the one who started the reading to ask.  What is not
// right is not used, it ends up as empty.
class c4_WalkGuard {
    const t4_byte *_prevEnd;

  public:
    // fresh: start with nothing wrong yet (else it is passed on to the outer)
    c4_WalkGuard(const t4_byte *end_, bool fresh_);
    ~c4_WalkGuard();

    static bool Bad();
    static void Fail();
    static bool Has(const t4_byte *ptr_, t4_i32 count_);
};

class c4_Column {
    c4_PtrArray _segments;
    t4_fpos _position;
    t4_i32 _size;
    c4_Persist *_persist;
    t4_i32 _gap;
    int _slack;
    bool _dirty;

  public:
    c4_Column(c4_Persist *persist_);
    //: Constructs a column using the specified persistence manager.
    ~c4_Column();

    void SetBuffer(t4_i32);
    //: Allocate a new buffer of the specified size.

    c4_Persist *Persist()const;
    //: Returns persistence manager for this column, or zero.
    c4_Strategy &Strategy()const;
    //: Returns the associated strategy pointer.
    t4_fpos Position()const;
    //: Special access for the DUMP program.
    t4_i32 ColSize()const;
    //: Returns the number of bytes as stored on disk.
    bool IsDirty()const;
    //: Returns true if contents needs to be saved.

    void SetLocation(t4_fpos, t4_i32);
    //: Sets the position and size of this column on file.
    void PullLocation(const t4_byte * &ptr_);
    //: Extract position and size of this column.

    int AvailAt(t4_i32 offset_)const;
    //: Returns number of bytes we can access at once.
    const t4_byte *LoadNow(t4_i32);
    //: Makes sure the data is loaded into memory.
    t4_byte *CopyNow(t4_i32);
    //: Makes sure a copy of the data is in memory.
    void Grow(t4_i32, t4_i32);
    //: Grows the buffer by inserting space.
    void Shrink(t4_i32, t4_i32);
    //: Shrinks the buffer by removing space.
    void SaveNow(c4_Strategy &, t4_fpos pos_);
    //: Save the buffer to file.

    const t4_byte *FetchBytes(t4_i32 pos_, int len_, c4_Bytes &buffer_, bool
      forceCopy_);
    //: Returns pointer to data, use buffer only if non-contiguous.
    void StoreBytes(t4_i32 pos_, const c4_Bytes &buffer_);
    //: Stores a copy of the buffer in the column.

    bool RequiresMap()const;
    void ReleaseAllSegments();

    static t4_i32 PullValue(const t4_byte * &ptr_);
    static void PushValue(t4_byte * &ptr_, t4_i32 v_);
    // same encoding, but for positions in a datafile, which may be wide
    static t4_fpos PullValue64(const t4_byte * &ptr_);
    static void PushValue64(t4_byte * &ptr_, t4_fpos v_);

    void InsertData(t4_i32 index_, t4_fpos count_, bool clear_);
    void RemoveData(t4_i32 index_, t4_i32 count_);

    void CheckGrow(t4_fpos extra_)const;
    //: Reports an error if the column cannot grow by this many bytes.
    void RemoveGap();

    enum {
        kSegBits = 12, kSegMax = 1 << kSegBits, kSegMask = kSegMax - 1
    };

  private:
    // (these are for positions in the buffers, which include the gap)
    static int fSegIndex(t4_cpos offset_);
    static t4_cpos fSegOffset(int index_);
    static int fSegRest(t4_cpos offset_);

    bool UsesMap(const t4_byte*)const;
    bool IsMapped()const;

    void ReleaseSegment(int);
    void SetupSegments();
    void Validate()const;
    void FinishSlack();

    void MoveGapUp(t4_i32 pos_);
    void MoveGapDown(t4_i32 pos_);
    void MoveGapTo(t4_i32 pos_);

    t4_byte *CopyData(t4_cpos, t4_cpos, int);
    void TooLarge(t4_fpos wanted_)const;
};

/////////////////////////////////////////////////////////////////////////////

class c4_ColOfInts: public c4_Column {
  public:
    c4_ColOfInts(c4_Persist *persist_, int width_ = sizeof(t4_i32));

    int RowCount()const;
    void SetRowCount(int numRows_);

    void FlipBytes();

    int ItemSize(int index_);
    const void *Get(int index_, int &length_);
    void Set(int index_, const c4_Bytes &buf_);

    t4_i32 GetInt(int index_);
    void SetInt(int index_, t4_i32 value_);

    void Insert(int index_, const c4_Bytes &buf_, int count_);
    void Remove(int index_, int count_);

    static int CalcAccessWidth(int numRows_, t4_i32 colSize_);

    void SetAccessWidth(int bits_);
    void FixSize(bool fudge_);
    void ForceFlip();

    static int DoCompare(const c4_Bytes &b1_, const c4_Bytes &b2_);

  private:
    typedef void(c4_ColOfInts:: *tGetter)(int);
    typedef bool(c4_ColOfInts:: *tSetter)(int, const t4_byte*);

    void Get_0b(int index_);
    void Get_1b(int index_);
    void Get_2b(int index_);
    void Get_4b(int index_);
    void Get_8i(int index_);
    void Get_16i(int index_);
    void Get_16r(int index_);
    void Get_32i(int index_);
    void Get_32r(int index_);
    void Get_64i(int index_);
    void Get_64r(int index_);

    bool Set_0b(int index_, const t4_byte *item_);
    bool Set_1b(int index_, const t4_byte *item_);
    bool Set_2b(int index_, const t4_byte *item_);
    bool Set_4b(int index_, const t4_byte *item_);
    bool Set_8i(int index_, const t4_byte *item_);
    bool Set_16i(int index_, const t4_byte *item_);
    bool Set_16r(int index_, const t4_byte *item_);
    bool Set_32i(int index_, const t4_byte *item_);
    bool Set_32r(int index_, const t4_byte *item_);
    bool Set_64i(int index_, const t4_byte *item_);
    bool Set_64r(int index_, const t4_byte *item_);

    void ResizeData(int index_, int count_, bool clear_ = false);

    tGetter _getter;
    tSetter _setter;

    union {
        t4_byte _item[8]; // holds temp result (careful with alignment!)
        double _aligner; // needed for SPARC
    };

    int _currWidth; // number of bits used for one entry (0..64)
    int _dataWidth; // number of bytes used for passing a value along
    int _numRows;
    bool _mustFlip;
};

/////////////////////////////////////////////////////////////////////////////

class c4_ColIter {
    c4_Column &_column;
    t4_i32 _limit;
    t4_i32 _pos;
    int _len;
    const t4_byte *_ptr;

  public:
    c4_ColIter(c4_Column &col_, t4_i32 offset_, t4_i32 limit_);
    //  ~c4_ColIter ();

    bool Next();
    bool Next(int max_);

    const t4_byte *BufLoad()const;
    t4_byte *BufSave();
    int BufLen()const;
};

/////////////////////////////////////////////////////////////////////////////

#if q4_INLINE
#include "column.inl"
#endif 

/////////////////////////////////////////////////////////////////////////////

#endif
