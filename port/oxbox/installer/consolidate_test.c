/* consolidate_test STOCK_INDEX MAP...: what consolidating these maps would save, on a PC, for testing. Built (in this folder) with
	cc -O2 -DSTOCKMAPS_HOST -o consolidate_test consolidate_test.c consolidate.c mapstream.c sha256.c -lz */
#include <stdio.h>
#include "consolidate.h"

int main(int argc, char **argv)
{
	struct consolidate_measure r;
	char error[256] = "";
	if (argc < 3)
		return 2;
	if (consolidate_measure(argv[1], (char const *const *)(argv + 2), argc - 2, NULL, &r, error))
	{
		printf("FAILED: %s\n", error);
		return 1;
	}
	printf("maps %d unreadable %d already %d before %llu in_stock %llu in_pack %llu own %llu after %llu\n", r.maps, r.unreadable,
		r.already, r.before, r.in_stock, r.in_pack, r.own, r.after);
	return 0;
}
