/* stockmaps_test HALO_FOLDER look|convert|restore (or build SOURCE DESTINATION campaign|nocampaign): what the installer does to a Halo folder's stock maps, on a PC, for
testing (the folder holds maps\, with the 13 retail multiplayer maps and 10 levels; STOCKMAPS_TEST_CACHE=DIR,DIR,DIR: folders
standing in for the cache drives). Built (in this folder) with
	cc -O2 -DSTOCKMAPS_HOST -o stockmaps_test stockmaps_test.c stockmaps.c mapstream.c sha256.c -lz */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stockmaps.h"

static void say(int warning, char const *text)
{
	printf("%s%s\n", warning ? "! " : "", text);
	fflush(stdout);
}

static void log_line(char const *text)
{
	printf("  %s\n", text);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct stockmaps_look look;
	char error[256] = "";
	int failed = 0;
	if (argc == 4 && !strcmp(argv[1], "rebuild"))
	{
		/* stockmaps_test rebuild H1U_FOLDER DESTINATION: Halo's retail maps rebuilt from Halo 1 Unlimited's, elsewhere */
		failed = stockmaps_rebuild(argv[2], argv[3], say, error);
		printf(failed ? "failed: %s\n" : "rebuilt\n", error);
		return failed;
	}
	if (argc == 5 && !strcmp(argv[1], "build"))
	{
		/* stockmaps_test build SOURCE DESTINATION campaign|nocampaign: the own-folder install's maps */
		failed = stockmaps_build(argv[2], argv[3], !strcmp(argv[4], "campaign"), say, error);
		if (failed)
			printf("failed: %s\n", error);
		else
			printf("built\n");
		return failed;
	}
	if (argc != 3)
	{
		fprintf(stderr, "usage: stockmaps_test HALO_FOLDER look|convert|restore\n");
		return 2;
	}
	stockmaps_log = log_line;
	/* (STOCKMAPS_TEST_CACHE=DIR[,DIR[,DIR]]: folders standing in for the cache drives) */
	if (getenv("STOCKMAPS_TEST_CACHE"))
	{
		static char caches[512];
		char *at = caches;
		int n = 0;
		snprintf(caches, sizeof(caches), "%s", getenv("STOCKMAPS_TEST_CACHE"));
		while (at && n < STOCKMAPS_CACHES)
		{
			char *comma = strchr(at, ',');
			if (comma)
				*comma = 0;
			stockmaps_cache[n++] = at;
			at = comma ? comma + 1 : NULL;
		}
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
