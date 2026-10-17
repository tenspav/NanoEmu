/* MP3 tags (ID3v2 + ID3v1 + MPEG header) in integer C, no malloc. */
#ifndef MDB_TAGS_H
#define MDB_TAGS_H
#include <stdint.h>
#include <stddef.h>
#define MDB_TAG_STR 256
typedef struct {
    char title[MDB_TAG_STR], artist[MDB_TAG_STR], album[MDB_TAG_STR], genre[MDB_TAG_STR], composer[MDB_TAG_STR], album_artist[MDB_TAG_STR];
    uint32_t year, track, disc;
    uint32_t sample_rate, bitrate_kbps, duration_ms, channels, layer, mpeg;
    uint8_t vbr;
    /* extras: long texts point into xbuf */
    uint32_t total_tracks, total_discs, bpm, compilation, gapless_album, explicit_flag /*0 none / 1 explicit / 2 clean*/, rating /*0..100*/;
    int64_t release_date;            /* unix s (UTC); 0 = none */
    const char *comment, *grouping, *subtitle, *description, *copyright, *content_provider, *lyrics;
    uint32_t lyrics_frames, lyrics_flag;
    char ignored[256];               /* fields a full tagger would store but this tool does not (warning) */
    /* work memory for long texts: the caller may supply (xbuf/xcap) BEFORE calling; otherwise a static 1 MB buffer is used
       (overwritten on every call). Must be >= 400 KB. */
    char *xbuf; uint32_t xcap;
} mdb_tags_t;
/* b = whole MP3 file (or prefix >= tag + 64 KB plus a 128 B tail); 0 = ok, -1 = no MPEG frame.
   xbuf/xcap (if != 0) are preserved; the rest of *t is zeroed. */
int mdb_tags_read(const uint8_t *b, size_t len, mdb_tags_t *t);
/* Prefix variant: b[0..len) = file prefix (whole ID3v2 tag + >= 70 KB of audio) + the last 128 B of the file; total = real size.
   If total == len it is the same as mdb_tags_read. For a compacted ID3v2 tag (large frames removed), pass total - bytes_removed. */
int mdb_tags_read_ex(const uint8_t *b, size_t len, size_t total, mdb_tags_t *t);
#endif
