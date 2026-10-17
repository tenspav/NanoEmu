/*
 * nano7_diskfs.c -- edits to the emulated iPod's FAT32 data volume, made as
 * an overlay of whole 4 KB blocks by FTL LBA (served like nand-patch=, see
 * nano7_fmc.c), never written to the disk image. Two edits, for
 * NanoEmu.app's music import:
 *
 *   app      lay a homebrew .hbapp over the clusters of an existing app file
 *            (e.g. /Apps/Executables/Nanosync.hbapp). The new file must not
 *            be larger; an HRL1 blob is padded inside its image section to
 *            the old size, so the directory entry and the FAT stay as they
 *            are (tools/disk_patch.py does the same).
 *   import   add every regular file of a host folder to a folder of the
 *            volume (e.g. /Apps/Data/Importar, where the NanoSync app
 *            imports MP3s from), with a long name and an 8.3 alias, on free
 *            clusters taken from the END of the volume, both FATs updated
 *            (tools/disk_import.py does the same). The OS's FAT cache in the
 *            RAM snapshot holds the sectors it used last; the end of the FAT
 *            is never among them. Files already in the folder are skipped.
 *
 * The volume: MBR in block 0 of the image, the first partition's start in
 * 4 KB blocks (63 on the snap6 device), 4 KB sectors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "nano7_diskfs.h"

#define BLK 4096

typedef struct Fs {
    nano7_diskfs_read_fn rd;
    void *opaque;
    int64_t lba0;               /* FTL LBA of the image's block 0 */
    uint64_t base;              /* partition start, bytes into the image */
    uint32_t bps, spc, csize, nfats, fatsz, root, nclus;
    uint64_t fat_off, data_off; /* bytes into the partition */
    GHashTable *out;            /* FTL LBA -> 4 KB block: the edits */
} Fs;

static uint64_t fs_lba(Fs *fs, uint64_t poff)
{
    return fs->lba0 + (fs->base + poff) / BLK;
}

/* the block at partition byte offset poff (4 KB aligned): edits first */
static bool fs_block(Fs *fs, uint64_t poff, uint8_t *blk)
{
    uint64_t lba = fs_lba(fs, poff);
    uint8_t *e = g_hash_table_lookup(fs->out, &lba);

    if (e) {
        memcpy(blk, e, BLK);
        return true;
    }
    return fs->rd(fs->opaque, lba, blk);
}

static void fs_put(Fs *fs, uint64_t poff, const uint8_t *blk)
{
    uint64_t *k = g_new(uint64_t, 1);

    *k = fs_lba(fs, poff);
    g_hash_table_replace(fs->out, k, g_memdup2(blk, BLK));
}

static bool fs_read(Fs *fs, uint64_t poff, void *buf, size_t len)
{
    uint8_t blk[BLK];
    uint8_t *p = buf;

    while (len) {
        uint64_t b = poff & ~(uint64_t)(BLK - 1);
        size_t o = poff - b, n = MIN(len, BLK - o);

        if (!fs_block(fs, b, blk)) {
            return false;
        }
        memcpy(p, blk + o, n);
        p += n;
        poff += n;
        len -= n;
    }
    return true;
}

static uint64_t fs_coff(Fs *fs, uint32_t c)
{
    return fs->data_off + (uint64_t)(c - 2) * fs->csize;
}

static uint32_t fat_get(Fs *fs, uint32_t c)
{
    uint8_t v[4];

    if (!fs_read(fs, fs->fat_off + 4ull * c, v, 4)) {
        return 0x0fffffff;
    }
    return ldl_le_p(v) & 0x0fffffff;
}

static void fat_set(Fs *fs, uint32_t c, uint32_t val)
{
    uint8_t blk[BLK];
    uint32_t k;

    for (k = 0; k < fs->nfats; k++) {
        uint64_t off = fs->fat_off + (uint64_t)k * fs->fatsz * fs->bps +
                       4ull * c;
        uint64_t b = off & ~(uint64_t)(BLK - 1);

        if (!fs_block(fs, b, blk)) {
            continue;
        }
        stl_le_p(blk + (off - b),
                 (ldl_le_p(blk + (off - b)) & 0xf0000000) | val);
        fs_put(fs, b, blk);
    }
}

static GArray *fs_chain(Fs *fs, uint32_t c)
{
    GArray *a = g_array_new(false, false, sizeof(uint32_t));

    while (c >= 2 && c < 0x0ffffff8 && a->len < fs->nclus) {
        g_array_append_val(a, c);
        c = fat_get(fs, c);
    }
    return a;
}

static bool fs_open(Fs *fs)
{
    uint8_t blk[BLK];
    uint32_t start, totsec, rsv;

    if (!fs->rd(fs->opaque, fs->lba0, blk) || lduw_le_p(blk + 510) != 0xaa55) {
        return false;
    }
    start = ldl_le_p(blk + 0x1c6);          /* partition 1, in 4 KB blocks */
    fs->base = (uint64_t)(start ? start : 63) * BLK;
    if (!fs_block(fs, 0, blk) || lduw_le_p(blk + 510) != 0xaa55) {
        return false;
    }
    fs->bps = lduw_le_p(blk + 11);
    fs->spc = blk[13];
    rsv = lduw_le_p(blk + 14);
    fs->nfats = blk[16];
    totsec = ldl_le_p(blk + 32);
    fs->fatsz = ldl_le_p(blk + 36);
    fs->root = ldl_le_p(blk + 44);
    if (fs->bps != BLK || !fs->spc || !fs->nfats || !fs->fatsz) {
        return false;
    }
    fs->csize = fs->spc * fs->bps;
    fs->fat_off = (uint64_t)rsv * fs->bps;
    fs->data_off = (uint64_t)(rsv + fs->nfats * fs->fatsz) * fs->bps;
    fs->nclus = (totsec - (rsv + fs->nfats * fs->fatsz)) / fs->spc + 2;
    return true;
}

/* ------------------------------------------------------------ directories */
typedef struct Dent {
    char *name;                 /* UTF-8 */
    uint32_t clus, size;
    uint8_t attr;
} Dent;

/* the raw bytes of a directory (all its clusters) */
static uint8_t *dir_load(Fs *fs, GArray *chain)
{
    uint8_t *d = g_malloc((size_t)chain->len * fs->csize);
    guint i;

    for (i = 0; i < chain->len; i++) {
        fs_read(fs, fs_coff(fs, g_array_index(chain, uint32_t, i)),
                d + (size_t)i * fs->csize, fs->csize);
    }
    return d;
}

static void dent_free(gpointer p)
{
    Dent *e = p;

    g_free(e->name);
    g_free(e);
}

static GPtrArray *dir_list(Fs *fs, uint32_t dirc)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(dent_free);
    g_autoptr(GArray) chain = fs_chain(fs, dirc);
    g_autofree uint8_t *d = dir_load(fs, chain);
    size_t len = (size_t)chain->len * fs->csize, i;
    gunichar2 lfn[20 * 13 + 1];
    int have = 0;

    memset(lfn, 0, sizeof(lfn));
    for (i = 0; i + 32 <= len; i += 32) {
        uint8_t *e = d + i;

        if (e[0] == 0) {
            break;
        }
        if (e[0] == 0xe5) {
            have = 0;
            continue;
        }
        if (e[11] == 0x0f) {
            int seq = (e[0] & 0x1f) - 1, k;
            static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22,
                                         24, 28, 30 };

            if (seq < 0 || seq >= 20) {
                continue;
            }
            if (e[0] & 0x40) {
                memset(lfn, 0, sizeof(lfn));
            }
            for (k = 0; k < 13; k++) {
                lfn[seq * 13 + k] = lduw_le_p(e + pos[k]);
            }
            have = 1;
            continue;
        }
        {
            Dent *de = g_new0(Dent, 1);
            int k;

            if (have) {
                for (k = 0; k < 20 * 13 && lfn[k] && lfn[k] != 0xffff; k++) {
                }
                lfn[k] = 0;
                de->name = g_utf16_to_utf8(lfn, -1, NULL, NULL, NULL);
            }
            if (!de->name) {
                char sh[13];
                int n = 0;

                for (k = 0; k < 8 && e[k] != ' '; k++) {
                    sh[n++] = e[k];
                }
                if (e[8] != ' ') {
                    sh[n++] = '.';
                    for (k = 8; k < 11 && e[k] != ' '; k++) {
                        sh[n++] = e[k];
                    }
                }
                sh[n] = 0;
                de->name = g_strdup(sh);
            }
            de->clus = lduw_le_p(e + 20) << 16 | lduw_le_p(e + 26);
            de->size = ldl_le_p(e + 28);
            de->attr = e[11];
            g_ptr_array_add(out, de);
            have = 0;
        }
    }
    return out;
}

static bool name_eq(const char *a, const char *b)
{
    g_autofree char *x = g_utf8_casefold(a, -1);
    g_autofree char *y = g_utf8_casefold(b, -1);

    return !strcmp(x, y);
}

static gint name_cmp(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(char *const *)a, *(char *const *)b);
}

/* the entry at an absolute path (NULL: not found); dir: its directory */
static Dent *fs_lookup(Fs *fs, const char *path, uint32_t *dir)
{
    g_auto(GStrv) parts = g_strsplit(path, "/", -1);
    uint32_t c = fs->root;
    Dent *found = NULL;
    int i;

    for (i = 0; parts[i]; i++) {
        GPtrArray *l;
        guint k;

        if (!*parts[i]) {
            continue;
        }
        if (found) {
            if (!(found->attr & 0x10)) {
                dent_free(found);
                return NULL;
            }
            c = found->clus;
            dent_free(found);
            found = NULL;
        }
        l = dir_list(fs, c);
        for (k = 0; k < l->len; k++) {
            Dent *e = g_ptr_array_index(l, k);

            if (name_eq(e->name, parts[i])) {
                found = g_new(Dent, 1);
                *found = *e;
                found->name = g_strdup(e->name);
                break;
            }
        }
        g_ptr_array_unref(l);
        if (!found) {
            return NULL;
        }
        if (dir) {
            *dir = c;
        }
    }
    return found;
}

/* ------------------------------------------------------------------ app */
/* grow an HRL1 blob {magic, entry, image_size, span, nrelocs, align} to
 * exactly size bytes with zeros at the end of its image section */
static bool hrl1_pad(GByteArray *b, uint32_t size)
{
    uint32_t image, span, extra = size - b->len;
    static const uint8_t zero[256];

    if (b->len < 24 || memcmp(b->data, "HRL1", 4) || extra % 4) {
        return false;
    }
    image = ldl_le_p(b->data + 8);
    span = ldl_le_p(b->data + 12);
    if (24 + (uint64_t)image > b->len) {
        return false;
    }
    stl_le_p(b->data + 8, image + extra);
    stl_le_p(b->data + 12, MAX(span, image + extra));
    {
        GByteArray *n = g_byte_array_sized_new(size);
        uint32_t left = extra;

        g_byte_array_append(n, b->data, 24 + image);
        while (left) {
            uint32_t k = MIN(left, sizeof(zero));

            g_byte_array_append(n, zero, k);
            left -= k;
        }
        g_byte_array_append(n, b->data + 24 + image, b->len - 24 - image);
        g_byte_array_set_size(b, 0);
        g_byte_array_append(b, n->data, n->len);
        g_byte_array_unref(n);
    }
    return true;
}

static void fs_install_app(Fs *fs, const char *path, const char *local)
{
    g_autofree char *data = NULL;
    gsize len;
    Dent *e = fs_lookup(fs, path, NULL);
    GByteArray *b;
    g_autoptr(GArray) chain = NULL;
    uint32_t i;
    uint8_t blk[BLK];

    if (!e || (e->attr & 0x10)) {
        fprintf(stderr, "[nano7-diskfs] app %s: not on the disk\n", path);
        goto out;
    }
    if (!g_file_get_contents(local, &data, &len, NULL)) {
        fprintf(stderr, "[nano7-diskfs] app %s: cannot read\n", local);
        goto out;
    }
    if (len > e->size) {
        fprintf(stderr, "[nano7-diskfs] app %s: %zu bytes > %s (%u bytes)\n",
                local, (size_t)len, path, e->size);
        goto out;
    }
    b = g_byte_array_new();
    g_byte_array_append(b, (uint8_t *)data, len);
    if (len < e->size && !hrl1_pad(b, e->size)) {
        uint32_t k = e->size - b->len;

        while (k--) {
            g_byte_array_append(b, (const uint8_t *)"", 1);
        }
    }
    chain = fs_chain(fs, e->clus);
    for (i = 0; i < e->size; i += BLK) {
        uint32_t ci = i / fs->csize, n = MIN(BLK, e->size - i);
        uint64_t off;

        if (ci >= chain->len) {
            break;
        }
        off = fs_coff(fs, g_array_index(chain, uint32_t, ci)) +
              i % fs->csize;
        fs_block(fs, off, blk);            /* keeps the slack after EOF */
        memcpy(blk, b->data + i, n);
        fs_put(fs, off, blk);
    }
    fprintf(stderr, "[nano7-diskfs] app %s <- %s (%zu bytes)\n", path, local,
            (size_t)len);
    g_byte_array_unref(b);
out:
    if (e) {
        dent_free(e);
    }
}

/* --------------------------------------------------------------- import */
static uint32_t alloc_from_end(Fs *fs, uint32_t *next)
{
    while (*next >= 2) {
        uint32_t c = (*next)--;

        if (fat_get(fs, c) == 0) {
            return c;
        }
    }
    return 0;
}

static uint8_t lfn_sum(const uint8_t *s)
{
    uint8_t v = 0;
    int i;

    for (i = 0; i < 11; i++) {
        v = ((v & 1) << 7) + (v >> 1) + s[i];
    }
    return v;
}

static void dos_time(time_t t, uint16_t *tm, uint16_t *dt)
{
    struct tm lt;

    localtime_r(&t, &lt);
    *dt = (lt.tm_year - 80) << 9 | (lt.tm_mon + 1) << 5 | lt.tm_mday;
    *tm = lt.tm_hour << 11 | lt.tm_min << 5 | lt.tm_sec / 2;
}

/* write the directory bytes back (all clusters of its chain) */
static void dir_store(Fs *fs, GArray *chain, const uint8_t *d)
{
    guint i;
    uint32_t j;

    for (i = 0; i < chain->len; i++) {
        uint64_t off = fs_coff(fs, g_array_index(chain, uint32_t, i));

        for (j = 0; j < fs->csize; j += BLK) {
            fs_put(fs, off + j, d + (size_t)i * fs->csize + j);
        }
    }
}

static void fs_import(Fs *fs, const char *path, const char *local)
{
    Dent *de = fs_lookup(fs, path, NULL);
    g_autoptr(GDir) gd = NULL;
    GPtrArray *names, *have;
    GArray *chain;
    uint8_t *d;
    uint32_t next = fs->nclus - 1, nadd = 0;
    uint16_t tm, dt;
    guint i, k;
    uint8_t blk[BLK];

    if (!de || !(de->attr & 0x10)) {
        fprintf(stderr, "[nano7-diskfs] import: folder %s not on the disk\n",
                path);
        if (de) {
            dent_free(de);
        }
        return;
    }
    gd = g_dir_open(local, 0, NULL);
    if (!gd) {
        dent_free(de);
        return;
    }
    names = g_ptr_array_new_with_free_func(g_free);
    for (const char *n; (n = g_dir_read_name(gd)); ) {
        g_autofree char *p = g_build_filename(local, n, NULL);

        if (n[0] != '.' && g_file_test(p, G_FILE_TEST_IS_REGULAR)) {
            g_ptr_array_add(names, g_strdup(n));
        }
    }
    g_ptr_array_sort(names, name_cmp);
    have = dir_list(fs, de->clus);
    chain = fs_chain(fs, de->clus);
    d = dir_load(fs, chain);
    dos_time(time(NULL), &tm, &dt);

    for (i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        g_autofree char *p = g_build_filename(local, name, NULL);
        g_autofree char *data = NULL;
        g_autofree gunichar2 *u = NULL;
        glong ulen = 0;
        gsize len;
        uint32_t ncl, first = 0, prev = 0, c, j, nlfn, need, run, pos;
        uint8_t sh[11];
        const char *dot;
        bool dup = false, ok = true;
        size_t off;

        for (k = 0; k < have->len; k++) {
            if (name_eq(((Dent *)g_ptr_array_index(have, k))->name, name)) {
                dup = true;
            }
        }
        u = g_utf8_to_utf16(name, -1, NULL, &ulen, NULL);
        if (dup || !u || ulen > 255 ||
            !g_file_get_contents(p, &data, &len, NULL) || len > 0xffffffffu) {
            continue;
        }
        /* clusters, from the end of the volume, ascending in the chain */
        ncl = MAX(1, (len + fs->csize - 1) / fs->csize);
        {
            g_autofree uint32_t *cl = g_new(uint32_t, ncl);

            for (j = 0; j < ncl && ok; j++) {
                cl[ncl - 1 - j] = c = alloc_from_end(fs, &next);
                ok = c != 0;
                if (ok) {
                    fat_set(fs, c, 0x0fffffff);     /* taken */
                }
            }
            if (!ok) {
                fprintf(stderr, "[nano7-diskfs] import: disk full at %s\n",
                        name);
                break;
            }
            for (j = 0; j < ncl; j++) {
                uint32_t b;

                c = cl[j];
                if (prev) {
                    fat_set(fs, prev, c);
                } else {
                    first = c;
                }
                prev = c;
                for (b = 0; b < fs->csize; b += BLK) {
                    size_t o = (size_t)j * fs->csize + b;

                    memset(blk, 0, BLK);
                    if (o < len) {
                        memcpy(blk, data + o, MIN(BLK, len - o));
                    }
                    fs_put(fs, fs_coff(fs, c) + b, blk);
                }
            }
            fat_set(fs, prev, 0x0fffffff);
        }
        /* 8.3 alias NSIMnnnn.EXT, unique in the folder */
        dot = strrchr(name, '.');
        for (j = 0; j < 10000; j++) {
            char base[9];
            bool used = false;

            snprintf(base, sizeof(base), "NSIM%04u", j);
            memcpy(sh, base, 8);
            memset(sh + 8, ' ', 3);
            for (k = 0; dot && dot[1 + k] && k < 3; k++) {
                sh[8 + k] = g_ascii_toupper(dot[1 + k]);
            }
            for (off = 0; off + 32 <= (size_t)chain->len * fs->csize;
                 off += 32) {
                if (d[off] && d[off] != 0xe5 && d[off + 11] != 0x0f &&
                    !memcmp(d + off, sh, 11)) {
                    used = true;
                }
            }
            if (!used) {
                break;
            }
        }
        /* a run of free entries for the long name pieces + the alias */
        nlfn = (ulen + 12) / 13;
        need = nlfn + 1;
        run = 0;
        pos = UINT32_MAX;
        for (off = 0; off + 32 <= (size_t)chain->len * fs->csize; off += 32) {
            if (d[off] == 0 || d[off] == 0xe5) {
                if (++run == need) {
                    pos = off - (need - 1) * 32;
                    break;
                }
            } else {
                run = 0;
            }
        }
        if (pos == UINT32_MAX) {            /* grow the folder by a cluster */
            uint32_t nc = alloc_from_end(fs, &next);
            size_t old = (size_t)chain->len * fs->csize;

            if (!nc) {
                break;
            }
            fat_set(fs, g_array_index(chain, uint32_t, chain->len - 1), nc);
            fat_set(fs, nc, 0x0fffffff);
            g_array_append_val(chain, nc);
            d = g_realloc(d, old + fs->csize);
            memset(d + old, 0, fs->csize);
            for (off = old; off > 0 && (d[off - 32] == 0); off -= 32) {
            }
            pos = off;
        }
        {
            bool end_used = d[pos + (need - 1) * 32] == 0;
            uint8_t sum = lfn_sum(sh);
            uint32_t s;
            static const int lpos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22,
                                          24, 28, 30 };

            for (s = nlfn; s >= 1; s--) {          /* last piece first */
                uint8_t *e = d + pos + (nlfn - s) * 32;

                memset(e, 0, 32);
                e[0] = s | (s == nlfn ? 0x40 : 0);
                e[11] = 0x0f;
                e[13] = sum;
                for (j = 0; j < 13; j++) {
                    glong ci = (glong)(s - 1) * 13 + j;
                    uint16_t ch = ci < ulen ? u[ci] : ci == ulen ? 0 : 0xffff;

                    stw_le_p(e + lpos[j], ch);
                }
            }
            {
                uint8_t *e = d + pos + nlfn * 32;

                memset(e, 0, 32);
                memcpy(e, sh, 11);
                e[11] = 0x20;
                stw_le_p(e + 14, tm);
                stw_le_p(e + 16, dt);
                stw_le_p(e + 18, dt);
                stw_le_p(e + 20, first >> 16);
                stw_le_p(e + 22, tm);
                stw_le_p(e + 24, dt);
                stw_le_p(e + 26, first & 0xffff);
                stl_le_p(e + 28, len);
            }
            if (end_used && pos + need * 32 < (size_t)chain->len * fs->csize) {
                d[pos + need * 32] = 0;            /* keep an end marker */
            }
        }
        {
            Dent *n = g_new0(Dent, 1);

            n->name = g_strdup(name);
            g_ptr_array_add(have, n);
        }
        nadd++;
        fprintf(stderr, "[nano7-diskfs] import %s/%s (%zu bytes)\n", path,
                name, (size_t)len);
    }
    dir_store(fs, chain, d);
    fprintf(stderr, "[nano7-diskfs] import: %u file(s) from %s\n", nadd,
            local);
    g_free(d);
    g_array_unref(chain);
    g_ptr_array_unref(have);
    g_ptr_array_unref(names);
    dent_free(de);
}

GHashTable *nano7_diskfs_build(nano7_diskfs_read_fn rd, void *opaque,
                               int64_t lba0, const char *app_path,
                               const char *app_file, const char *import_path,
                               const char *import_dir)
{
    Fs fs = { .rd = rd, .opaque = opaque, .lba0 = lba0 };

    fs.out = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                   g_free);
    if (!fs_open(&fs)) {
        fprintf(stderr, "[nano7-diskfs] no FAT32 volume with 4 KB sectors "
                "on the disk image\n");
        return fs.out;
    }
    if (app_path && app_file) {
        fs_install_app(&fs, app_path, app_file);
    }
    if (import_path && import_dir) {
        fs_import(&fs, import_path, import_dir);
    }
    return fs.out;
}
