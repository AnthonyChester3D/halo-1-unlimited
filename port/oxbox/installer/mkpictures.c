/* mkpictures ui.map [folder]: OpenCE's own menu pictures (pictures.h) for a build, made from the ui.map given (the
game's own, from your disc or hard disk): writes pictures.bin and online_games.dxt3 into the folder (port/oxbox by
default, where tools/mkxbe.py looks). The installer makes the same on the Xbox. Built by port/oxbox/build.sh:
	cc -O2 -ffp-contract=off -o build-xbox/mkpictures port/oxbox/installer/mkpictures.c \
		port/oxbox/installer/pictures.c -lz -lm */
#include "pictures.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char pictures[PICTURES_SIZE], header[HEADER_SIZE];

static int save(char const *folder, char const *name, unsigned char const *data, size_t size)
{
	char path[4096];
	FILE *file;
	text_print(path, sizeof(path), "%s/%s", folder, name);
	file = fopen(path, "wb");
	if (!file || fwrite(data, 1, size, file) != size || fclose(file))
	{
		fprintf(stderr, "mkpictures: can't write %s\n", path);
		return 1;
	}
	printf("%s (%lu bytes)\n", path, (unsigned long)size);
	return 0;
}

int main(int argc, char **argv)
{
	char error[256];
	char const *folder = argc > 2 ? argv[2] : "port/oxbox";
	if (argc < 2)
	{
		fprintf(stderr, "usage: mkpictures ui.map [folder]\n");
		return 2;
	}
	if (pictures_make(argv[1], pictures, header, error))
	{
		fprintf(stderr, "mkpictures: %s\n", error);
		return 1;
	}
	return save(folder, "pictures.bin", pictures, sizeof(pictures)) ||
		save(folder, "online_games.dxt3", header, sizeof(header));
}
