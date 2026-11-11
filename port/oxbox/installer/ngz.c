/* ngz.c: zlib-ng behind a small wall (ngz.h) */
#include "ngz.h"

#include <stdlib.h>
#include <zlib-ng.h>

struct ngz
{
	zng_stream z;
	int deflating;
};

struct ngz *ngz_deflate(int level)
{
	struct ngz *z = calloc(1, sizeof(*z));
	if (!z)
		return NULL;
	z->deflating = 1;
	if (zng_deflateInit2(&z->z, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
	{
		free(z);
		return NULL;
	}
	return z;
}

struct ngz *ngz_inflate(void)
{
	struct ngz *z = calloc(1, sizeof(*z));
	if (!z)
		return NULL;
	if (zng_inflateInit(&z->z) != Z_OK)
	{
		free(z);
		return NULL;
	}
	return z;
}

int ngz_run(struct ngz *z, int flush, unsigned char const **in, uint32_t *in_size, unsigned char **out, uint32_t *out_size)
{
	int status;
	z->z.next_in = *in;
	z->z.avail_in = *in_size;
	z->z.next_out = *out;
	z->z.avail_out = *out_size;
	status = z->deflating ? zng_deflate(&z->z, flush) : zng_inflate(&z->z, flush);
	*in = z->z.next_in;
	*in_size = z->z.avail_in;
	*out = z->z.next_out;
	*out_size = z->z.avail_out;
	return status;
}

void ngz_end(struct ngz *z)
{
	if (!z)
		return;
	if (z->deflating)
		zng_deflateEnd(&z->z);
	else
		zng_inflateEnd(&z->z);
	free(z);
}
