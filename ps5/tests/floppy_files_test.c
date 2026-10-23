// PC test of fe_floppy.c's FAT reader: lists the image's files and copies each out (sha checked by the script).
#include "fe_floppy.h"
#include <stdio.h>

int main(int argc, char** argv)
{
	static fe_floppy_file f[64];
	const int n = fe_floppy_files(argv[1], f, 64);
	printf("files: %d\n", n);
	for (int i = 0; i < n; i++)
	{
		char out[512];
		snprintf(out, sizeof(out), "%s/f%d.bin", argv[2], i);
		const int r = fe_floppy_extract(argv[1], f[i].cluster, f[i].size, out);
		printf("%s|%u|%s|%d\n", f[i].name, f[i].size, out, r);
	}
	return 0;
}
