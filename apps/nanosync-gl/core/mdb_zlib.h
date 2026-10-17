/* Mini zlib: full inflate (stored/fixed/dynamic) + writer with stored blocks. No malloc, integers only. */
#ifndef MDB_ZLIB_H
#define MDB_ZLIB_H
#include <stdint.h>
#include <stddef.h>
/* Descomprime fluxo zlib (78 xx ... adler32). 0 = ok; *olen = bytes gerados.
   Uses the output itself as the window. *consumed (optional) = input bytes read. */
int mdb_inflate(const uint8_t *in, size_t ilen, uint8_t *out, size_t ocap, size_t *olen, size_t *consumed);
/* Maximum size of a "stored" zlib stream for n bytes. */
size_t mdb_zstore_bound(size_t n);
/* Write a zlib stream with stored blocks; returns bytes written. */
size_t mdb_zstore(const uint8_t *in, size_t n, uint8_t *out);
uint32_t mdb_adler32(const uint8_t *p, size_t n);
#endif
