/* Minimal SHA-1, integers only, no malloc. */
#ifndef MDB_SHA1_H
#define MDB_SHA1_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t h[5]; uint64_t n; uint8_t buf[64]; uint32_t used; } mdb_sha1_t;
void mdb_sha1_init(mdb_sha1_t *c);
void mdb_sha1_update(mdb_sha1_t *c, const void *d, size_t n);
void mdb_sha1_final(mdb_sha1_t *c, uint8_t out[20]);
void mdb_sha1(const void *d, size_t n, uint8_t out[20]);
#endif
