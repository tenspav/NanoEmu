/* Music database signatures (HashAB) - integers only, no malloc. */
#ifndef MDB_SIG_H
#define MDB_SIG_H
#include <stdint.h>
#include <stddef.h>
/* 57 B HashAB of a SHA1 (20 B) + FireWire GUID (8 B). */
void mdb_hashab(uint8_t out[57], const uint8_t sha1[20], const uint8_t guid[8]);
/* Sign a physical iTunesCDB/iTunesDB in memory. 0 = ok */
int mdb_cdb_sign(uint8_t *buf, size_t len, const uint8_t guid[8]);
/* Check the signature (1 = valid). Uses 244 B of internal scratch (does not copy the file). */
int mdb_cdb_verify(const uint8_t *buf, size_t len, const uint8_t guid[8]);
/* Locations.itdb.cbk: size = 57+20+20*nblocks. For slices/streaming use the pieces below. */
size_t mdb_cbk_size(size_t loc_len);
/* Compute the whole cbk from the whole Locations.itdb in memory. out >= mdb_cbk_size. */
void mdb_cbk_build(uint8_t *out, const uint8_t *loc, size_t loc_len, const uint8_t guid[8]);
#endif
