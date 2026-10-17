/* Text: UTF-8/UTF-16LE and sort-key normalization (integers only, no locale libc). */
#ifndef MDB_TEXT_H
#define MDB_TEXT_H
#include <stdint.h>
#include <stddef.h>
uint32_t mdb_u8next(const char **pp);
size_t mdb_u8put(char *o, uint32_t c);
/* UTF-8 -> UTF-16LE (out may be NULL to measure). Returns bytes. */
size_t mdb_u8_to_u16le(const char *s, uint8_t *out);
/* UTF-16LE -> zero-terminated UTF-8 (out NULL = measure only; NUL not counted). */
size_t mdb_u16le_to_u8(const uint8_t *in, size_t nbytes, char *out);
/* Sort key: removes leading "the "/"an "/"a ", NFKD, casefold.
   Coverage: ASCII, Latin-1, Latin Extended-A (casefold), basic Greek and Cyrillic. out NULL = measure. */
size_t mdb_norm_key(const char *s, char *out);
/* simple casefold (to compare composers). */
size_t mdb_casefold(const char *s, char *out);
/* Index jump letter for an already normalized key. */
uint32_t mdb_jump_letter(const char *key);
#endif
