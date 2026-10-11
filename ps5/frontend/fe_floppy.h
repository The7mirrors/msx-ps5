// MSX PS5 (2026-10-10, AI-assisted): USB floppy drives.
//
// A USB floppy drive is a USB mass-storage device: the PS5's kernel gives it a disk device (/dev/daN) like any USB
// drive. A floppy disk is told from the console's USB hard drives by its size (160 KB to 2.88 MB, a whole number
// of 512-byte sectors). The disk is only ever read, sector by sector, into a .dsk image -- MSX disks are raw
// 720 KB (or 360 KB) sector dumps, the very format the core loads. Nothing is written back to the disk: what the
// game writes stays in the core's save file for that image.
//
// SPDX-License-Identifier: MIT
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FE_FLOPPY_MAX 4

// The floppy drives with a disk in them right now: their devices ("/dev/da3") and the disks' sizes. Returns how many.
int fe_floppy_list(char devs[][32], long long sizes[], int max);

// Reads the whole disk in `dev` into `out_path` (a .dsk). Returns the number of sectors that could not be read
// (filled with zeros), -1 when the drive or the file can't be opened, -2 when no sector at all could be read. *hash: a hash of the disk's contents, to
// name the image (the same disk gets the same name, and so the same save file).
int fe_floppy_read(const char* dev, const char* out_path, unsigned long long* hash);

#ifdef __cplusplus
}
#endif
