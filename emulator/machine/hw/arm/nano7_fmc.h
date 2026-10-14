/*
 * nano7_fmc.h -- minimal stateful PPN NAND flash controller (FMC) model
 * for the iPod nano 7G machine. See nano7_fmc.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_FMC_H
#define NANO7_FMC_H

#include "exec/memory.h"
#include "hw/irq.h"

void nano7_fmc_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq);
/* ctx resume: mark the sequence in flight at capture time as finished */
void nano7_fmc_fmss_resume_done(void);
/* ctx resume: the next kick with no sequence base (armed before capture)
 * completes successfully */
void nano7_fmc_fmss_resume_kick(void);

/* NAND page store: store = backing file (NULL: temp file), miss =
 * "blank"|"oracle", disk = 4 KB-sector disk image for oracle misses,
 * disk_off = FTL LBA of its sector 0 (a disk-mode dd of the iPod is the
 * data volume only: MBR + FAT32 partition; the image starts at FTL LBA
 * 49216 (0xc040) on the snap6 device) */
void nano7_fmc_nand_config(const char *store, const char *miss,
                           const char *disk, const char *disk_off);

/* save state: copy the NAND page store out to / back in from a file */
bool nano7_fmc_store_save(const char *path);
bool nano7_fmc_store_load(const char *path);

#endif
