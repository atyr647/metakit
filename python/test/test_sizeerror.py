# test_sizeerror.py -- A column which would grow too large is a Python exception
# $Id$
# This is part of Metakit, see http://www.equi4.com/metakit/

# A column can not be larger than 2^31-1 bytes (see c4_SizeError).  To get to
# that without needing 2 Gb, the limit is made lower while this runs (it is a
# variable of the library, which is for testing: f4_MaxColumnSize).  What must
# happen is that it is an exception of the type Mk4py.SizeError, that nothing
# was changed, that what is in the datafile is not harmed, and that the
# storage can be used again after a rollback.

from mktestsupport import *
import Mk4py
import os
import sys

try:
    import ctypes
except ImportError:
    ctypes = None
    print 'test_sizeerror: skipped, needs ctypes'

if ctypes is not None:
    lib = ctypes.CDLL(os.path.abspath(Mk4py.__file__))
    limit = ctypes.c_int.in_dll(lib, 'f4_MaxColumnSize')
    NORMAL = limit.value
    FILE = 'test_sizeerror.mk'
    failures = Failure()

    def check(what, ok):
        if not ok:
            failures.fail(what, ())

    def raises(exc, func, *args, **kw):
        try:
            func(*args, **kw)
        except exc:
            return True
        except:
            print 'wrong exception:', sys.exc_info()[:2]
            return False
        return False

    def rows(storage, name, props):
        # what is in a datafile, read by a new storage
        s2 = metakit.storage(FILE, 0)
        v = s2.view(name)
        result = [tuple([getattr(r, p) for p in props]) for r in v]
        del v, s2
        return result

    def fresh():
        if os.path.exists(FILE):
            os.remove(FILE)

    try:
        # 1. appending beyond the limit, which taints the storage
        fresh()
        db = metakit.storage(FILE, 1)
        v = db.getas('t[s:S]')
        v.append(s='x' * 100)
        db.commit()

        limit.value = 1000
        v.append(s='y' * 600)  # 700 bytes in the column, fine
        check('append below limit', len(v) == 2)
        check('error is a SizeError', raises(Mk4py.SizeError, v.append,
              s='z' * 600))
        check('SizeError is an exception', issubclass(Mk4py.SizeError,
              Exception))
        check('nothing was added', len(v) == 2)
        check('rows are intact', v[0].s == 'x' * 100 and v[1].s == 'y' * 600)
        check('a commit is refused until rollback', raises(IOError,
              db.commit))

        limit.value = NORMAL
        db.rollback()
        v = db.view('t')
        check('rollback restores the committed rows', len(v) == 1 and
              v[0].s == 'x' * 100)
        v.append(s='after')
        db.commit()
        del v, db
        check('what is in the datafile', rows(None, 't', ['s']) ==
              [('x' * 100,), ('after',)])

        # 2. rows with several columns are all or nothing
        fresh()
        db = metakit.storage(FILE, 1)
        v = db.getas('t[a:S,b:S,n:I]')
        v.append(a='a', b='b', n=1)
        db.commit()
        limit.value = 500
        # the second column is the one which does not fit: 3 x 200 > 500
        v.append(a='1', b='q' * 200, n=2)
        v.append(a='2', b='q' * 200, n=3)
        check('several columns, error', raises(Mk4py.SizeError, v.append,
              a='3', b='q' * 200, n=4))
        check('several columns, same number of rows', len(v) == 3)
        check('several columns, rows readable', [(r.a, len(r.b), r.n) for r
              in v] == [('a', 1, 1), ('1', 200, 2), ('2', 200, 3)])
        limit.value = NORMAL
        db.rollback()
        db.commit()
        del v, db
        check('several columns, datafile', rows(None, 't', ['a', 'n']) ==
              [('a', 1)])

        # 3. every way of changing things meets the limit without a crash
        fresh()
        db = metakit.storage(FILE, 1)
        v = db.getas('t[s:S,b:B,sub[x:S]]')
        v.append(s='x' * 100, b='b' * 100)
        db.commit()
        limit.value = 400
        count = 0
        cur = [v]

        def op_append():
            cur[0].append(s='y' * 150, b='c' * 150)

        def op_insert():
            cur[0].insert(0, dict(s='y' * 150))

        def op_setitem():
            cur[0][0] = dict(s='w' * 390, b='d' * 390)

        def op_setattr():
            cur[0][0].s = 'v' * 390

        def op_subview():
            cur[0][0].sub.append(x='u' * 390)

        def op_setslice():
            cur[0][0:1] = [dict(s='t' * 150)] * 3

        ops = [op_append, op_insert, op_setitem, op_setattr, op_subview,
               op_setslice]
        for i in range(60):
            try:
                ops[i % len(ops)]()
            except Mk4py.SizeError:
                count += 1
                limit.value = NORMAL
                db.rollback()
                cur[0] = db.view('t')
                limit.value = 400
        limit.value = NORMAL
        check('operations met the limit', count > 0)
        db.rollback()
        db.commit()
        del v, db
    finally:
        limit.value = NORMAL
        fresh()

    failures.assess()
