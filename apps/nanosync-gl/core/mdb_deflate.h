#ifndef MDB_DEFLATE_H
#define MDB_DEFLATE_H
#include <stdint.h>
#include <stddef.h>
size_t mdb_zdeflate_bound(size_t n);
/* zlib stream with one fixed Huffman block; returns bytes written (0 = error/no space) */
size_t mdb_zdeflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
#define MDB_DEFLATE_WORK ((1u << 15) * 4u * 2u)   /* head[32768] + prev[32768] of int32 = 256 KB */
size_t mdb_zdeflate_w(const uint8_t *in, size_t n, uint8_t *out, size_t cap, void *work, size_t worksz);
#endif
