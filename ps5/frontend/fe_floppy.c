// MSX PS5 (2026-10-10, AI-assisted): USB floppy drives (fe_floppy.h).
// SPDX-License-Identifier: MIT

#include "fe_floppy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#if defined(__has_include)
#if __has_include(<sys/disk.h>)
#include <sys/disk.h>
#endif
#endif
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

// the sizes of floppy disks: 160 KB (5.25" single-sided) up to 2.88 MB (3.5" ED); USB hard drives are far bigger
static int is_floppy_size(long long size)
{
	return size >= 160 * 1024 && size <= 2949120 && (size % 512) == 0;
}

// the disk's size: the device's media size; a plain file's size (an image, on the PC tests)
static long long media_size(int fd)
{
#ifdef DIOCGMEDIASIZE
	off_t size = 0;
	if (ioctl(fd, DIOCGMEDIASIZE, &size) == 0)
		return (long long)size;
#endif
	struct stat st;
	return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? (long long)st.st_size : -1;
}

int fe_floppy_list(char devs[][32], long long sizes[], int max)
{
	int n = 0;
	for (int i = 0; i < 16 && n < max; i++)
	{
		char dev[32];
		snprintf(dev, sizeof(dev), "/dev/da%d", i);
		struct stat st;
		if (stat(dev, &st) != 0)
			continue;
		const int fd = open(dev, O_RDONLY);
		if (fd < 0)
			continue;
		const long long size = media_size(fd);
		if (is_floppy_size(size))
		{
			snprintf(devs[n], 32, "%s", dev);
			sizes[n] = size;
			n++;
		}
		close(fd);
	}
	return n;
}

int fe_floppy_read(const char* dev, const char* out_path, unsigned long long* hash)
{
	const int fd = open(dev, O_RDONLY);
	if (fd < 0)
		return -1;
	const off_t size = (off_t)media_size(fd);
	if (!is_floppy_size((long long)size))
	{
		close(fd);
		return -1;
	}
	FILE* out = fopen(out_path, "wb");
	if (!out)
	{
		close(fd);
		return -1;
	}
	unsigned long long h = 1469598103934665603ull;
	int bad = 0;
	// a track at a time (18 sectors of a 1.44 MB disk, 9 of a 720 KB one: 9 KB divides both); a track that fails
	// is read again a sector at a time, so one bad sector costs only itself
	unsigned char track[9 * 1024];
	for (off_t pos = 0; pos < size; pos += (off_t)sizeof(track))
	{
		const size_t want = (size_t)(size - pos) < sizeof(track) ? (size_t)(size - pos) : sizeof(track);
		ssize_t got = pread(fd, track, want, pos);
		if (got != (ssize_t)want)
		{
			for (size_t s = 0; s < want; s += 512)
			{
				int ok = 0;
				for (int attempt = 0; attempt < 3 && !ok; attempt++)
					ok = pread(fd, track + s, 512, pos + (off_t)s) == 512;
				if (!ok)
				{
					memset(track + s, 0, 512);
					bad++;
				}
			}
		}
		for (size_t i = 0; i < want; i++)
			h = (h ^ track[i]) * 1099511628211ull;
		if (fwrite(track, 1, want, out) != want)
		{
			fclose(out);
			close(fd);
			remove(out_path);
			return -1;
		}
	}
	close(fd);
	if (fclose(out) != 0)
	{
		remove(out_path);
		return -1;
	}
	// nothing readable at all (a blank, worn-out or wrong-density disk, or dirty drive heads): no image
	if (bad >= (int)(size / 512))
	{
		remove(out_path);
		return -2;
	}
	if (hash)
		*hash = h;
	return bad;
}

// ---- MSX PS5 (2026-10-15): a PC-formatted floppy's files (FAT12 / FAT16) ----
typedef struct
{
	int fd;
	unsigned bps, spc, clusters, fat16;
	unsigned root_lba, root_count, data_lba;
	unsigned char* fat; // the first FAT, whole
	unsigned fat_bytes;
	long long size;
} fat_disk;

// whole sectors (a device reads only those), a few tries each
static int read_sectors(const fat_disk* d, unsigned lba, unsigned count, unsigned char* out)
{
	const size_t want = (size_t)count * d->bps;
	if ((long long)(lba + count) * d->bps > d->size)
		return -1;
	for (int attempt = 0; attempt < 3; attempt++)
		if (pread(d->fd, out, want, (off_t)lba * d->bps) == (ssize_t)want)
			return 0;
	return -1;
}

static unsigned fat_next(const fat_disk* d, unsigned c)
{
	if (d->fat16)
		return 2 * c + 1 < d->fat_bytes ? (unsigned)(d->fat[2 * c] | d->fat[2 * c + 1] << 8) : 0xFFFF;
	const unsigned off = c + c / 2;
	if (off + 1 >= d->fat_bytes)
		return 0xFFF;
	const unsigned v = d->fat[off] | d->fat[off + 1] << 8;
	return (c & 1) ? v >> 4 : v & 0xFFF;
}
static int fat_end(const fat_disk* d, unsigned c)
{
	return c < 2 || c >= d->clusters + 2 || c >= (d->fat16 ? 0xFFF8u : 0xFF8u);
}

static int fat_open(fat_disk* d, const char* dev)
{
	memset(d, 0, sizeof(*d));
	d->fd = open(dev, O_RDONLY);
	if (d->fd < 0)
		return -1;
	d->size = media_size(d->fd);
	unsigned char b[512];
	if (d->size < 512 || pread(d->fd, b, 512, 0) != 512)
		return -1;
	const unsigned bps = b[11] | b[12] << 8, spc = b[13], reserved = b[14] | b[15] << 8, nfats = b[16];
	const unsigned root_entries = b[17] | b[18] << 8, spf = b[22] | b[23] << 8;
	unsigned total = b[19] | b[20] << 8;
	if (!total)
		total = b[32] | b[33] << 8 | b[34] << 16 | (unsigned)b[35] << 24;
	// a FAT boot sector: sizes that make sense (an MSX disk of its own, or a blank one, has none of these)
	if ((bps != 512 && bps != 1024 && bps != 2048) || !spc || (spc & (spc - 1)) || !reserved || nfats < 1 || nfats > 2 ||
		!root_entries || !spf || !total || (long long)total * bps > d->size + bps)
		return 0;
	d->bps = bps, d->spc = spc;
	d->root_lba = reserved + nfats * spf;
	d->root_count = (root_entries * 32 + bps - 1) / bps;
	d->data_lba = d->root_lba + d->root_count;
	if (d->data_lba >= total)
		return 0;
	d->clusters = (total - d->data_lba) / spc;
	d->fat16 = d->clusters >= 4085;
	if (d->clusters >= 65525)
		return 0; // (FAT32: not a floppy)
	d->fat_bytes = spf * bps;
	d->fat = (unsigned char*)malloc(d->fat_bytes);
	if (!d->fat || read_sectors(d, reserved, spf, d->fat) != 0)
		return -1;
	return 1;
}
static void fat_close(fat_disk* d)
{
	free(d->fat);
	if (d->fd >= 0)
		close(d->fd);
	d->fat = NULL, d->fd = -1;
}

// a long name's piece: 13 UCS-2 characters as UTF-8, into its place (seq 1 = the start)
static void lfn_piece(char lfn[][40], const unsigned char* e)
{
	const int seq = (e[0] & 0x1F);
	if (seq < 1 || seq > 20)
		return;
	static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
	char* o = lfn[seq - 1];
	int n = 0;
	for (int i = 0; i < 13; i++)
	{
		const unsigned c = e[at[i]] | e[at[i] + 1] << 8;
		if (c == 0 || c == 0xFFFF)
			break;
		if (c < 0x80)
			o[n++] = (char)c;
		else if (c < 0x800)
			o[n++] = (char)(0xC0 | c >> 6), o[n++] = (char)(0x80 | (c & 63));
		else
			o[n++] = (char)(0xE0 | c >> 12), o[n++] = (char)(0x80 | (c >> 6 & 63)), o[n++] = (char)(0x80 | (c & 63));
	}
	o[n] = 0;
}

static unsigned char lfn_sum(const unsigned char* e)
{
	unsigned char s = 0;
	for (int i = 0; i < 11; i++)
		s = (unsigned char)(((s & 1) << 7) + (s >> 1) + e[i]);
	return s;
}

static int fat_dir(fat_disk* d, unsigned cluster, const char* prefix, int depth, fe_floppy_file* out, int max, int n)
{
	// the folder's sectors: the root's fixed place, or a folder's clusters
	unsigned char* buf = NULL;
	unsigned bytes = 0;
	if (!cluster)
	{
		bytes = d->root_count * d->bps;
		buf = (unsigned char*)malloc(bytes);
		if (!buf || read_sectors(d, d->root_lba, d->root_count, buf) != 0)
		{
			free(buf);
			return n;
		}
	}
	else
	{
		const unsigned cbytes = d->spc * d->bps;
		for (unsigned c = cluster, steps = 0; !fat_end(d, c) && steps < 256; c = fat_next(d, c), steps++)
		{
			unsigned char* nb = (unsigned char*)realloc(buf, bytes + cbytes);
			if (!nb)
				break;
			buf = nb;
			if (read_sectors(d, d->data_lba + (c - 2) * d->spc, d->spc, buf + bytes) != 0)
				break;
			bytes += cbytes;
		}
	}
	char lfn[20][40];
	int have_lfn = 0;
	unsigned char lfn_check = 0;
	for (unsigned off = 0; off + 32 <= bytes && n < max; off += 32)
	{
		const unsigned char* e = buf + off;
		if (e[0] == 0)
			break;
		if (e[0] == 0xE5)
		{
			have_lfn = 0;
			continue;
		}
		if ((e[11] & 0x3F) == 0x0F)
		{
			if (e[0] & 0x40)
				memset(lfn, 0, sizeof(lfn)), have_lfn = 1, lfn_check = e[13];
			if (have_lfn && e[13] == lfn_check)
				lfn_piece(lfn, e);
			continue;
		}
		if (e[11] & 0x08) // the disk's label
		{
			have_lfn = 0;
			continue;
		}
		// the name: the long one when it belongs to this entry, else NAME.EXT
		char name[160];
		if (have_lfn && lfn_sum(e) == lfn_check)
		{
			name[0] = 0;
			for (int i = 0; i < 20 && lfn[i][0]; i++)
				strncat(name, lfn[i], sizeof(name) - strlen(name) - 1);
		}
		else
		{
			int k = 0;
			for (int i = 0; i < 8 && e[i] != ' '; i++)
				name[k++] = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
			if (e[8] != ' ')
			{
				name[k++] = '.';
				for (int i = 8; i < 11 && e[i] != ' '; i++)
					name[k++] = (char)e[i];
			}
			name[k] = 0;
		}
		have_lfn = 0;
		if (!name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
			continue;
		const unsigned first = e[26] | e[27] << 8 | (d->fat16 ? 0 : 0);
		char path[200];
		snprintf(path, sizeof(path), "%s%s", prefix, name);
		if (e[11] & 0x10)
		{
			if (depth < 3 && first >= 2)
			{
				char sub[210];
				snprintf(sub, sizeof(sub), "%s/", path);
				n = fat_dir(d, first, sub, depth + 1, out, max, n);
			}
			continue;
		}
		const unsigned size = e[28] | e[29] << 8 | e[30] << 16 | (unsigned)e[31] << 24;
		if (!size || first < 2)
			continue;
		snprintf(out[n].name, sizeof(out[n].name), "%s", path);
		out[n].cluster = first;
		out[n].size = size;
		n++;
	}
	free(buf);
	return n;
}

int fe_floppy_files(const char* dev, fe_floppy_file* out, int max)
{
	fat_disk d;
	const int r = fat_open(&d, dev);
	if (r <= 0)
	{
		fat_close(&d);
		return r;
	}
	const int n = fat_dir(&d, 0, "", 0, out, max, 0);
	fat_close(&d);
	return n;
}

int fe_floppy_extract(const char* dev, unsigned cluster, unsigned size, const char* out_path)
{
	fat_disk d;
	if (fat_open(&d, dev) <= 0 || size > 16u * 1024 * 1024)
	{
		fat_close(&d);
		return -1;
	}
	FILE* out = fopen(out_path, "wb");
	const unsigned cbytes = d.spc * d.bps;
	// MSX PS5 1.2 (2026-10-22): the file's clusters that follow one another on the disk are read together (up to 72 KB at
	// a time, four cylinders of a 1.44 MB disk), not one by one: a PC format has 512-byte clusters, and each read of its
	// own waited for the disk to come round (a 128 KB game took 55 s).
	const unsigned kRunMax = 144; // clusters per read
	unsigned char* buf = (unsigned char*)malloc((size_t)cbytes * kRunMax);
	unsigned left = size, steps = 0;
	int ok = out && buf;
	unsigned c = cluster;
	while (ok && left > 0)
	{
		if (fat_end(&d, c) || ++steps > d.clusters)
		{
			ok = 0; // the chain ends too soon, or loops
			break;
		}
		// a run: c, c+1, ... as long as the chain goes straight on and the file wants more
		unsigned run = 1, last = c;
		while (run < kRunMax && (unsigned long long)run * cbytes < left)
		{
			const unsigned next = fat_next(&d, last);
			if (fat_end(&d, next) || next != last + 1)
				break;
			last = next;
			run++;
			if (++steps > d.clusters)
				break;
		}
		if (read_sectors(&d, d.data_lba + (c - 2) * d.spc, run * d.spc, buf) != 0)
		{
			// a run that won't read whole: cluster by cluster (the bad one fails alone)
			for (unsigned i = 0; i < run; i++)
				if (read_sectors(&d, d.data_lba + (c + i - 2) * d.spc, d.spc, buf + (size_t)i * cbytes) != 0)
				{
					ok = 0; // a cluster can't be read
					break;
				}
			if (!ok)
				break;
		}
		const unsigned long long have = (unsigned long long)run * cbytes;
		const unsigned take = left < have ? left : (unsigned)have;
		ok = fwrite(buf, 1, take, out) == take;
		left -= take;
		c = fat_next(&d, last);
	}
	free(buf);
	if (out && fclose(out) != 0)
		ok = 0;
	fat_close(&d);
	if (!ok)
		remove(out_path);
	return ok ? 0 : -1;
}
