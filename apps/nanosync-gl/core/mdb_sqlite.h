/* Minimal SQLite file generator (format 4, UTF-8, 4096-byte pages) without libsqlite, malloc or float.
   Writes rowid tables + indexes (complete B-trees, with interior pages when needed). */
#ifndef MDB_SQLITE_H
#define MDB_SQLITE_H
#include <stdint.h>
#include <stddef.h>

typedef struct { uint8_t *base; size_t cap, used; } mdb_arena_t;
void *mdb_aalloc(mdb_arena_t *a, size_t n);

#include "mdb_schema.h"

enum { MV_NULL = 0, MV_INT = 1, MV_REAL = 2, MV_TEXT = 3 };
typedef struct { uint8_t t; int64_t i; const char *s; uint32_t n; } mval_t;   /* REAL: i = bits IEEE754 */
typedef struct { int64_t rowid; mval_t *v; } mrow_t;
typedef struct { mrow_t *rows; uint32_t n, cap; int64_t next_rowid; } mtab_t;

#define MDB_SQL_PAGE 4096
/* Write the file. tabs[ntab] holds the rows. 0 = ok, <0 error (-2 = no space, -3 = no memory, -4 = record too large). */
/* file change counter written to the header (default 1); set before mdb_sqlite_write */
extern uint32_t mdb_sqlite_change_counter;
int mdb_sqlite_write(const mdb_schema_t *sc, mtab_t *tabs, uint8_t *out, size_t cap, size_t *outlen, mdb_arena_t *ar);
uint64_t mdb_f64_from_u32(uint32_t v);   /* IEEE754 bits of (double)v, integers only */
#endif
