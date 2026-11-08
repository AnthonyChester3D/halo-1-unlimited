/* consolidate_test: consolidate.c on a PC, for testing. Built (in this folder) with
	cc -O2 -DSTOCKMAPS_HOST -o consolidate_test consolidate_test.c consolidate.c mapstream.c sha256.c -lz
consolidate_test STOCK_INDEX MAP...: what consolidating these maps would save.
consolidate_test --convert STOCK_FOLDER PACK_FOLDER PREFIX MAP...: converts them, as the installer does, each into
PACK_FOLDER/<its name>/ (made, with a map.link, as an import makes it), the pack's file in PACK_FOLDER. STOCK_FOLDER holds
stock.index and stock.shared. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "consolidate.h"

static void say(int warning, char const *text)
{
	fprintf(stderr, "%s%s\n", warning ? "! " : "", text);
}

/* (the installer's paths have backslashes) */
static char *backslashes(char const *from)
{
	char *to = strdup(from), *p;
	for (p = to; *p; p++)
		if (*p == '/')
			*p = '\\';
	return to;
}

int main(int argc, char **argv)
{
	char error[256] = "";
	if (argc >= 6 && !strcmp(argv[1], "--convert"))
	{
		struct consolidate_result r;
		int count = argc - 5, k;
		char const **paths = calloc((size_t)count, sizeof(*paths)), **folders = calloc((size_t)count, sizeof(*folders));
		for (k = 0; k < count; k++)
		{
			char folder[512], link[600], *name = strrchr(argv[5 + k], '/');
			FILE *f;
			name = name ? name + 1 : argv[5 + k];
			snprintf(folder, sizeof(folder), "%s/%.*s", argv[3], (int)(strlen(name) - 4), name);
			mkdir(folder, 0777);
			snprintf(link, sizeof(link), "%s/map.link", folder);
			if ((f = fopen(link, "w")))
				fprintf(f, "%s\r\n", argv[5 + k]), fclose(f);
			paths[k] = backslashes(argv[5 + k]);
			folders[k] = backslashes(folder);
		}
		if (consolidate_convert(backslashes(argv[2]), backslashes(argv[3]), argv[4], paths, folders, count, say, &r, error))
		{
			printf("FAILED: %s\n", error);
			return 1;
		}
		printf("converted %d already %d unreadable %d too_big %d failed %d before %llu after %llu shared %s\n", r.converted,
			r.already, r.unreadable, r.too_big, r.failed, r.before, r.after, r.shared);
		return 0;
	}
	if (argc < 3)
		return 2;
	{
		struct consolidate_measure r;
		if (consolidate_measure(argv[1], (char const *const *)(argv + 2), argc - 2, NULL, &r, error))
		{
			printf("FAILED: %s\n", error);
			return 1;
		}
		printf("maps %d unreadable %d already %d before %llu in_stock %llu in_pack %llu own %llu after %llu\n", r.maps,
			r.unreadable, r.already, r.before, r.in_stock, r.in_pack, r.own, r.after);
	}
	return 0;
}
