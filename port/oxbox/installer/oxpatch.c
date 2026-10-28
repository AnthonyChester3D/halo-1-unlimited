/* oxpatch package.oxpk RETAIL.xbe ui.map OUT.xbe: what the installer does, on a PC: makes the package's build of
OpenCE from a retail Halo XBE, its pictures from the ui.map given, and says who signed it (the keys in keys.txt). For
testing packages; built (in this folder) with
	python3 ../tools/mkkeys.py keys.txt keys.h
	cc -O2 -ffp-contract=off -I../../third_party/monocypher -o oxpatch oxpatch.c package.c pictures.c sha256.c \
		../../third_party/monocypher/monocypher.c ../../third_party/monocypher/monocypher-ed25519.c -lz -lm */
#include <stdio.h>
#include <stdlib.h>

#include "package.h"
#include "pictures.h"
#include "sha256.h"
#include "text.h"

#include "keys.h"

static int write_file(void *context, unsigned char const *data, unsigned long size)
{
	return fwrite(data, 1, size, (FILE *)context) != size;
}

static unsigned char pictures[PICTURES_SIZE], header[HEADER_SIZE];

int main(int argc, char **argv)
{
	struct package package;
	struct package_xbe const *xbe;
	struct package_patch patch;
	unsigned char *retail;
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
	{
		char const *signer;
		int signature = package_verify(&package, trusted_keys, trusted_key_count, &signer);
		printf("package: %s (format %d, release %lu)\n", package.name, package.format, package.release);
		printf("  %s%s\n", signature == PACKAGE_SIGNED ? "signed by " : signature == PACKAGE_UNSIGNED ? "not signed" :
			signature == PACKAGE_UNKNOWN_KEY ? "signed, but not by a key in keys.txt" :
			"SIGNATURE DOESN'T MATCH (changed after it was signed)", signature == PACKAGE_SIGNED ? signer : "");
		if (signature == PACKAGE_BAD)
			return 1;
	}
	for (i = 0; i < package.count; i++)
		printf("  makes a build from %s (%lu bytes -> %lu)\n", package.xbes[i].name, package.xbes[i].retail_size,
			package.xbes[i].build_size);
	if (package.installer_version)
		printf("  carries installer version %lu (%lu bytes)\n", package.installer_version, package.installer_size);
	file = fopen(argv[2], "rb");
	if (!file || fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) ||
		!(retail = malloc(size)) || fread(retail, 1, size, file) != (size_t)size)
	{
		fprintf(stderr, "oxpatch: can't read %s\n", argv[2]);
		return 1;
	}
	fclose(file);
	xbe = package_match(&package, retail, size, &patch);
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
	printf("%s: from %s%s\n", argv[4], xbe->name, patch.patched ? " (a copy patched for media or regions)" : "");
	return 0;
}
