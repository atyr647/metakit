// mk4io.h --
// $Id$
// This is part of Metakit, the homepage is http://www.equi4.com/metakit.html

/** @file
 * Declaration of the file stream and strategy classes.
 */

#ifndef __MK4IO_H__
#define __MK4IO_H__

#include <stdio.h>

/////////////////////////////////////////////////////////////////////////////
/// A file stream can be used to serialize using the stdio library.

class c4_FileStream: public c4_Stream {
  public:
    c4_FileStream(FILE *stream_, bool owned_ = false);
    virtual ~c4_FileStream();

    virtual int Read(void *buffer_, int length_);
    virtual bool Write(const void *buffer_, int length_);

    FILE *_stream;
    bool _owned;
};

/////////////////////////////////////////////////////////////////////////////
/// A file strategy encapsulates code dealing with all file I/O.

class c4_FileStrategy: public c4_Strategy {
  public:
    /// Construct a new strategy object
    c4_FileStrategy(FILE *file_ = 0);
    virtual ~c4_FileStrategy();

    /// True if we can do I/O with this object
    virtual bool IsValid()const;
    /// Open a data file by name.  When opened to write (mode 1 or 2) the file
    /// is locked for as long as it is open, and a writer which finds it locked
    /// does not open it: it is not valid, and _failure is kFailLocked.
    virtual bool DataOpen(const char *fileName_, int mode_);
    /// Read a number of bytes
    virtual int DataRead(t4_fpos pos_, void *buffer_, int length_);
    /// Write a number of bytes, return true if successful
    virtual void DataWrite(t4_fpos pos_, const void *buffer_, int length_);
    /// Flush and truncate file
    virtual void DataCommit(t4_fpos newSize_);
    /// Support for memory-mapped files
    virtual void ResetFileMapping();
    /// Report total size of the datafile
    virtual t4_fpos FileSize();
    /// Return a good value to use as fresh generation counter
    virtual t4_i32 FreshGeneration();
    /// Force everything written so far to the disk, see c4_Strategy
    virtual void DataSync();
    /// Take note of the size and the ends of the file, see c4_Strategy
    virtual void DataRemember();
    /// True if the size or the ends of the file are not as they were noted
    virtual bool DataChanged();

    /// Set to false before DataOpen to open for writing without the lock which
    /// keeps out other writers (see DataOpen), which METAKIT_NOLOCK also does
    bool _lock;

  protected:
    /// Pointer to file object
    FILE *_file;
    /// Pointer to same file object, if it must be deleted at end
    FILE *_cleanup;
    /// Size of the file and its ends as they were noted, see DataRemember
    t4_fpos _noteSize;
    t4_byte _noteEnds[32];
    bool _noted;

    bool ReadEnds(t4_fpos &size_, t4_byte *ends_);
    bool TakeLock();
};

/////////////////////////////////////////////////////////////////////////////

#endif // __MK4IO_H__
