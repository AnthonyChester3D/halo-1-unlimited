/* stockmaps_test HALO_FOLDER look|convert|restore: what the installer does to a Halo folder's stock maps, on a PC, for
testing (the folder holds maps\, with the 13 retail multiplayer maps and 10 levels). Built (in this folder) with
	cc -O2 -DSTOCKMAPS_HOST -o stockmaps_test stockmaps_test.c stockmaps.c mapstream.c sha256.c -lz */
#include <stdio.h>
#include <string.h>

#include "stockmaps.h"

static void say(int warning, char const *text)
{
	printf("%s%s\n", warning ? "! " : "", text);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct stockmaps_look look;
	char error[256] = "";
	int failed = 0;
	if (argc != 3)
	{
		fprintf(stderr, "usage: stockmaps_test HALO_FOLDER look|convert|restore\n");
		return 2;
	}
	if (!strcmp(argv[2], "convert"))
		failed = stockmaps_convert(argv[1], say, error);
	else if (!strcmp(argv[2], "restore"))
		failed = stockmaps_restore(argv[1], say, error);
	else if (strcmp(argv[2], "look"))
		return 2;
	if (failed)
		printf("FAILED: %s\n", error);
	stockmaps_examine(argv[1], &look);
	printf("retail sized %d, small %d, restorable %d\n", look.retail_sized, look.small, look.restorable);
	return failed;
}
