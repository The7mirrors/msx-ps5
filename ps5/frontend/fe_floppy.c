// MSX PS5 (2026-10-10, AI-assisted): USB floppy drives (fe_floppy.h).
// SPDX-License-Identifier: MIT

#include "fe_floppy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

// the sizes of floppy disks: 160 KB (5.25" single-sided) up to 2.88 MB (3.5" ED); USB hard drives are far bigger
static int is_floppy_size(long long size)
{
	return size >= 160 * 1024 && size <= 2949120 && (size % 512) == 0;
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
		off_t size = 0;
		if (ioctl(fd, DIOCGMEDIASIZE, &size) == 0 && is_floppy_size((long long)size))
		{
			snprintf(devs[n], 32, "%s", dev);
			sizes[n] = (long long)size;
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
	off_t size = 0;
	if (ioctl(fd, DIOCGMEDIASIZE, &size) != 0 || !is_floppy_size((long long)size))
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
