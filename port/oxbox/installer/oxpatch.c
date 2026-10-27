/* oxpatch package.oxpk RETAIL.xbe ui.map OUT.xbe: what the installer does, on a PC: makes the package's build of
OpenCE from a clean retail Halo XBE, its pictures from the ui.map given. (For testing packages; built with
	cc -O2 -ffp-contract=off -o oxpatch oxpatch.c package.c pictures.c sha256.c -lz -lm) */
#include <stdio.h>
#include <stdlib.h>

#include "package.h"
#include "pictures.h"
#include "sha256.h"
#include "text.h"

static int write_file(void *context, unsigned char const *data, unsigned long size)
{
	return fwrite(data, 1, size, (FILE *)context) != size;
}

static unsigned char pictures[PICTURES_SIZE], header[HEADER_SIZE];

int main(int argc, char **argv)
{
	struct package package;
	struct package_xbe const *xbe;
	unsigned char digest[32], *retail;
	char error[256];
	long size;
	FILE *file;
	int i;

	if (argc != 5)
	{
		fprintf(stderr, "usage: oxpatch package.oxpk RETAIL.xbe ui.map OUT.xbe\n");
		return 2;
	}
	if (package_load(argv[1], &package, error))
	{
		fprintf(stderr, "oxpatch: %s\n", error);
		return 1;
	}
	printf("package: %s\n", package.name);
	for (i = 0; i < package.count; i++)
		printf("  makes a build from %s (%lu bytes -> %lu)\n", package.xbes[i].name, package.xbes[i].retail_size,
			package.xbes[i].build_size);
	file = fopen(argv[2], "rb");
	if (!file || fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) ||
		!(retail = malloc(size)) || fread(retail, 1, size, file) != (size_t)size)
	{
		fprintf(stderr, "oxpatch: can't read %s\n", argv[2]);
		return 1;
	}
	fclose(file);
	sha256(retail, size, digest);
	xbe = package_find(&package, digest);
	if (!xbe)
	{
		fprintf(stderr, "oxpatch: %s isn't a retail XBE this package knows\n", argv[2]);
		return 1;
	}
	if (pictures_make(argv[3], pictures, header, error))
	{
		fprintf(stderr, "oxpatch: no pictures: %s\n", error);
		return 1;
	}
	file = fopen(argv[4], "wb");
	if (!file)
	{
		fprintf(stderr, "oxpatch: can't write %s\n", argv[4]);
		return 1;
	}
	if (package_make(&package, xbe, retail, pictures, header, write_file, file, error) || fclose(file))
	{
		fprintf(stderr, "oxpatch: %s\n", error);
		return 1;
	}
	printf("%s: from %s\n", argv[4], xbe->name);
	return 0;
}
