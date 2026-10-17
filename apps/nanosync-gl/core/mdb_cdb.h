/* iTunesCDB writer (iPod nano 7 music database): adds tracks to an existing logical CDB.
   Integers only, no malloc (work memory supplied by the caller). */
#ifndef MDB_CDB_H
#define MDB_CDB_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *title, *artist, *album, *genre, *composer, *album_artist; /* UTF-8; NULL = "" */
    const char *location;            /* "iPod_Control/Music/F00/ABCD.mp3" (with '/') */
    uint32_t year, track_no, disc_no;
    uint32_t length_ms, size, bitrate_kbps, sample_rate;
    uint8_t vbr;
    /* extras: NULL/0 = absent */
    const char *comment, *grouping, *subtitle, *description, *copyright, *content_provider, *lyrics;
    uint32_t total_tracks, total_discs, bpm, compilation, gapless_album, explicit_flag, rating, lyrics_flag;
    int64_t release_date;               /* unix s (UTC) */
    uint32_t artwork_id, artwork_count, artwork_size;   /* mhit 0x160, 0x7C, 0x80 (0 = no artwork) */
    uint32_t date_added, last_modified;   /* Unix seconds */
} mdb_track_t;

/* Assigned ids (returned for the SQLite generator and the tests). */
typedef struct {
    uint32_t track_id;
    uint64_t db_track_id;
    uint32_t album_id, artist_id, composer_id;
    uint64_t album_sql_id, artist_sql_id;  /* != 0 only when the track created the group */
    uint64_t mhip_pid[2];                  /* mhip ids in the type 3 and type 2 mhsd (document order) */
    uint32_t position;                     /* position in the master playlist */
} mdb_assigned_t;

enum {
    MDB_OK = 0, MDB_E_FORMAT = -1, MDB_E_SPACE = -2, MDB_E_SCRATCH = -3, MDB_E_UNSUPPORTED = -4, MDB_E_ARG = -5
};

/* Decompress a physical iTunesCDB to its logical form (header size fixed up, flag 0xA8 cleared). */
int mdb_cdb_unpack(const uint8_t *phys, size_t plen, uint8_t *logical, size_t cap, size_t *llen);
/* Pack the logical CDB into physical form (zlib body with stored blocks) and sign with HashAB. */
size_t mdb_cdb_pack_bound(size_t llen);
int mdb_cdb_pack(const uint8_t *logical, size_t llen, uint8_t *phys, size_t cap, size_t *plen, const uint8_t guid[8]);

/* Add n tracks to the logical CDB `in`. Output goes to `out` (cap bytes). `asg` (n items) receives the ids.
   scratch: work memory (mdb_cdb_scratch_need gives a safe estimate). */
size_t mdb_cdb_out_bound(size_t in_len, uint32_t n);
size_t mdb_cdb_scratch_need(size_t in_len, uint32_t n);
int mdb_cdb_add_tracks(const uint8_t *in, size_t in_len, const mdb_track_t *tr, uint32_t n,
                       uint8_t *out, size_t out_cap, size_t *out_len, mdb_assigned_t *asg,
                       void *scratch, size_t scratch_size);
#endif
