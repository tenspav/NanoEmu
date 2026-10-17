/*
 * nano7_diskfs.h -- edits to the emulated iPod's FAT32 data volume as an
 * overlay of 4 KB blocks (NanoEmu.app's music import). See nano7_diskfs.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_DISKFS_H
#define NANO7_DISKFS_H

/* read the current 4 KB block at an FTL LBA (false: unreadable) */
typedef bool (*nano7_diskfs_read_fn)(void *opaque, uint64_t lba,
                                     uint8_t *blk);

/* build the overlay (FTL LBA -> 4 KB block, g_free'd keys and values):
 * lba0 = FTL LBA of the disk image's block 0; app_file is laid over the
 * existing app_path; every file of the host folder import_dir is added to
 * the volume's folder import_path. Either pair may be NULL. */
GHashTable *nano7_diskfs_build(nano7_diskfs_read_fn rd, void *opaque,
                               int64_t lba0, const char *app_path,
                               const char *app_file, const char *import_path,
                               const char *import_dir);

#endif
