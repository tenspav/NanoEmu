/* NanoSync - sequential reader over FOBJ_READ. SHARED by the device (ns_hb.c) and by the simulator in "SDK semantic" mode
   (tests/ns_host.c): the simulator exercises exactly this code against a fake FOBJ_READ with the OS restrictions.

   Assumed FOBJ_READ semantics (none of them guaranteed by the SDK):
   - it may return FEWER bytes than requested with rc==0 (hence the loop);
   - when reading at/after end of file it may return rc != 0 and got == 0 (the EOF rc is undocumented);
   - the OS writes 64 bits into *got.
   ns_seq_read returns: bytes read (>= 0). End of file OR error with 0 bytes = 0; whoever knows the expected size (the engine, via fsize)
   tells "end" from "error" and the diagnostic (ns_iodiag) keeps the raw rc. Before the fix (NS_RD_LEGACY) rc != 0 with no bytes became -1,
   and the engine mistook the test read past the end (EOF) for a read error. */
#ifndef NS_RD_H
#define NS_RD_H
#include <stdint.h>
#include "ns_engine.h"
#ifndef NS_HAS_DIAG   /* old engine (only to reproduce the "before" error in the simulator) */
typedef struct { int32_t rc; uint32_t req, got, calls; } ns_iodiag;
#endif

typedef int (*ns_fobj_read_fn)(void *o, uint32_t n, void *buf, uint32_t *got);

static inline int32_t ns_seq_read(ns_fobj_read_fn rd0, void *obj, void *buf, uint32_t n, ns_iodiag *dg) {
    ns_fobj_read_fn volatile rd = rd0; uint32_t have = 0, calls = 0;
    dg->rc = 0; dg->req = n; dg->got = 0; dg->calls = 0;
#ifdef NS_RD_LEGACY
    while (have < n && calls < 64) {
        uint64_t got64 = 0; int rc = rd(obj, n - have, (uint8_t *)buf + have, (uint32_t *)(void *)&got64); uint32_t got = (uint32_t)got64;
        calls++; dg->rc = rc; dg->calls = calls;
        if (got > n - have) return -1;
        have += got; dg->got = have;
        if (rc != 0) return have ? (int32_t)have : -1;
        if (got == 0) break;
    }
    return (int32_t)have;
#else
    while (have < n) {                                   /* no call limit: every call returning got > 0 makes progress */
        uint64_t got64 = 0; int rc = rd(obj, n - have, (uint8_t *)buf + have, (uint32_t *)(void *)&got64); uint32_t got = (uint32_t)got64;
        calls++; dg->rc = rc; dg->calls = calls;
        if (got > n - have) { dg->got = have; return -1; }  /* the OS claims to have read more than requested: inconsistent */
        have += got; dg->got = have;
        if (rc != 0 || got == 0) break;                  /* end of file (or error): return whatever there is, including 0 */
    }
    return (int32_t)have;
#endif
}
#endif
