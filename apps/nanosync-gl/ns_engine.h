/* NanoSync - engine that imports MP3 files into the stock player's library (iPod nano 7).
   The SAME code runs on the device (ns_hb.c) and in the PC simulator (tests/ns_host.c). No libc, no threads,
   no malloc: a single memory block supplied by the caller. Time-sliced state machine (ns_step). */
#ifndef NS_ENGINE_H
#define NS_ENGINE_H
#include <stdint.h>
#include <stddef.h>
#include "mdb_tags.h"

#define NS_MAX_FILES   20u          /* MP3 files per run */
#define NS_NAME        128          /* file name (bytes) */
#define NS_PIECE       32768u       /* one I/O piece per unit of work */
#define NS_MSG         160
#define NS_MIN_BATT    40           /* minimum % to import */

/* Diagnostic of the platform's LAST I/O operation (raw SDK/OS return): rc = raw FOBJ_READ return (or FOBJ_READY after
   opening), req = bytes requested, got = bytes obtained in total for the request, calls = FOBJ_READ calls made. */
typedef struct { int32_t rc; uint32_t req, got, calls; } ns_iodiag;
#define NS_HAS_DIAG 1

/* Platform (the device uses hb_fs_ and FOBJ; the simulator uses a directory tree). Return 0 = ok in the int functions. */
typedef struct ns_os {
    int      (*exists)(const char *path);
    int32_t  (*fsize)(const char *path);                       /* -1 = does not exist */
    int      (*is_dir)(const char *path);
    int      (*mkdir)(const char *path);
    int      (*remove)(const char *path);
    uint32_t (*read_all)(const char *path, void *buf, uint32_t max);   /* small file, from the start; 0 = failure/empty */
    int      (*r_open)(const char *path);                      /* sequential reader (one at a time) */
    int32_t  (*r_read)(void *buf, uint32_t n);                 /* bytes read (< n only at end of file); <0 error */
    void     (*r_close)(void);
    int      (*w_open)(const char *path);                      /* sequential writer (one at a time; creates/truncates) */
    int      (*w_write)(const void *buf, uint32_t n);
    int      (*w_close)(void);
    int      (*d_open)(const char *path);                      /* directory listing (one at a time) */
    int      (*d_next)(char *name, int cap, int *is_dir);      /* 1 = entry, 0 = end */
    void     (*d_close)(void);
    void    *(*alloc)(uint32_t n);
    void     (*mfree)(void *p);
    uint32_t (*heap_free)(void);
    uint32_t (*heap_largest)(void);
    uint32_t (*now_us)(void);
    int      (*battery_pct)(void);                              /* 0..100 */
    uint32_t (*unix_time)(void);                                /* Unix seconds (UTC) */
    void     (*trace)(const char *tag, uint32_t a, uint32_t b);
    void     (*io_diag)(ns_iodiag *out);                       /* last I/O (may be NULL) */
} ns_os;

typedef enum {
    NS_P_INIT = 0, NS_P_RECOVER, NS_P_SCAN, NS_P_TAGS, NS_P_LIB, NS_P_READY,
    NS_P_CHECK, NS_P_PLAN, NS_P_BAK, NS_P_JRN, NS_P_COPY, NS_P_CPVER, NS_P_TAIL, NS_P_ART, NS_P_GEN, NS_P_WRITE, NS_P_VERIFY, NS_P_RMSRC, NS_P_JDEL,
    NS_P_DONE, NS_P_FAIL
} ns_phase;

/* Stages shown on the progress bar */
typedef enum { NS_ST_NONE = 0, NS_ST_COPY, NS_ST_GEN, NS_ST_WRITE, NS_ST_VERIFY } ns_stage;

/* Result of a run */
typedef enum {
    NS_R_NONE = 0, NS_R_OK, NS_R_DRYRUN_OK, NS_R_RESTORED, NS_R_REFUSED, NS_R_ROLLED_BACK, NS_R_CRITICAL
} ns_result;

typedef struct {
    char     name[NS_NAME];
    uint32_t size;
    int8_t   status;          /* 0 = ok; <0 = refused (see reason) */
    char     reason[40];
    uint8_t  have_tags;
    int8_t   cv;              /* cover: 0 = none, 1 = JPEG found, 2 = unsupported/ignored */
    uint8_t  cs;              /* slot of the decoded cover + 1 (0 = no cover) */
    char     dest[40];        /* "iPod_Control/Music/F00/ABCD.mp3" */
    uint8_t  sha[20];         /* SHA-1 of the source file (computed during the copy) */
} ns_file;

typedef struct ns ns_t;

uint32_t ns_state_bytes(void);                         /* minimum block bytes for the state only (no work area) */
uint32_t ns_min_block(void);                           /* minimum useful block */
/* Create the engine at the start of `block`. Returns NULL if the block is too small. */
ns_t *ns_create(const ns_os *os, void *block, uint32_t bytes);
/* One step (slices of up to budget_us). Returns 1 if there is more immediate work, 0 if waiting for the user / finished. */
int  ns_step(ns_t *s, uint32_t budget_us);
void ns_request_import(ns_t *s);                       /* user tapped Import */
void ns_request_rescan(ns_t *s);                       /* reload the list */
int  ns_can_cancel(const ns_t *s);

/* State for the UI */
ns_phase    ns_get_phase(const ns_t *s);
ns_stage    ns_get_stage(const ns_t *s);
uint32_t    ns_get_progress(const ns_t *s);            /* 0..100 of the current stage */
const char *ns_get_msg(const ns_t *s);                 /* status line, ASCII, short English */
const char *ns_get_diag(const ns_t *s);                /* last error: code + raw return + requested/obtained + file (ASCII, short lines); "" if none */
const char *ns_get_msg2(const ns_t *s);                /* detail / error */
ns_result   ns_get_result(const ns_t *s);
uint32_t    ns_nfiles(const ns_t *s);
const ns_file *ns_file_at(const ns_t *s, uint32_t i);
const mdb_tags_t *ns_tags_at(const ns_t *s, uint32_t i);
void ns_label(const ns_t *s, uint32_t i, char *out, uint32_t cap, char *out2, uint32_t cap2);   /* title / artist in ASCII */
int  ns_dryrun(const ns_t *s);
int  ns_lib_before(const ns_t *s);                     /* track count in the library (-1 = unknown) */
int  ns_lib_after(const ns_t *s);
uint32_t ns_mem_need(const ns_t *s);                   /* estimated/measured work memory (bytes) */
uint32_t ns_mem_have(const ns_t *s);
uint32_t ns_mem_peak(const ns_t *s);                   /* real peak usage of the work area */
uint32_t ns_block_bytes(const ns_t *s);
const char *ns_log_text(const ns_t *s, uint32_t *len);

/* Test / measurement information */
typedef struct {
    uint32_t t_gen_us, t_tags_us, t_cdb_us, t_sql_us, t_defl_us, t_sign_us, t_cover_us;   /* CPU times (platform clock) */
    uint32_t max_unit_us;                                                      /* largest unit of work in one slice */
    uint32_t cdb_phys_in, cdb_logical_in, cdb_logical_out, cdb_phys_out, sql_len[3], cbk_len;
    uint32_t scratch_cdb_used, scratch_sql_used;
} ns_stats;
const ns_stats *ns_get_stats(const ns_t *s);

/* Paths */
#define NS_DIR_IMP   "/Apps/Data/Importar"
#define NS_F_JOURNAL "/Apps/Data/Importar/journal.txt"
#define NS_F_LOG     "/Apps/Data/Importar/log.txt"
#define NS_F_DRY     "/Apps/Data/Importar/dryrun.txt"
#define NS_F_NOART   "/Apps/Data/Importar/noart.txt"
#define NS_F_INIT    "/Apps/Data/Importar/ns_init.txt"
#define NS_DIR_ITUNES "/iPod_Control/iTunes"
#define NS_DIR_ART   "/iPod_Control/Artwork"
#define NS_DIR_ITL   "/iPod_Control/iTunes/iTunes Library.itlp"

#endif
