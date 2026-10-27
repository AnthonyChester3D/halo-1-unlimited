/* package.c: an OpenCE package (package.h; port/oxbox/tools/mkpackage.py has its format) */
#include "package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "pictures.h"
#include "sha256.h"
#include "text.h"

#define FORMAT 1
#define HEADER_BYTES 128
enum { END, COPY, POOL, ZERO, PICTURES, HEADER };

static unsigned long u32(unsigned char const *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* (zlib built without its own allocator on the Xbox: nxdk's Z_SOLO) */
static voidpf z_alloc(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return calloc(items, size);
}

static void z_free(voidpf opaque, voidpf address)
{
	(void)opaque;
	free(address);
}

static void text(char *to, unsigned char const *from, int size)
{
	int i;
	for (i = 0; i < size && from[i]; i++)
		to[i] = from[i] >= 32 && from[i] < 127 ? (char)from[i] : '?';
	to[i] = 0;
}

/* walks an XBE's steps, checking each; its size made, or 0 if they're bad */
static unsigned long steps_check(struct package const *package, struct package_xbe const *xbe)
{
	unsigned char const *p = package->body + xbe->steps, *end = p + xbe->steps_size;
	unsigned long made = 0;
	for (;;)
	{
		unsigned long offset, size;
		int op;
		if (p >= end)
			return 0;
		op = *p++;
		if (op == END)
			return p == end ? made : 0;
		if (op == COPY || op == POOL)
		{
			if (end - p < 8)
				return 0;
			offset = u32(p), size = u32(p + 4);
			p += 8;
			if (offset > (op == COPY ? xbe->retail_size : package->body_size) ||
				size > (op == COPY ? xbe->retail_size : package->body_size) - offset)
				return 0;
		}
		else if (op == ZERO || op == PICTURES || op == HEADER)
		{
			if (end - p < 4)
				return 0;
			size = u32(p);
			p += 4;
			if ((op == PICTURES && size != PICTURES_SIZE) || (op == HEADER && size != HEADER_SIZE))
				return 0;
		}
		else
			return 0;
		if (size > xbe->build_size - made)
			return 0;
		made += size;
	}
}

int package_load(char const *path, struct package *package, char *error)
{
	FILE *file = fopen(path, "rb");
	unsigned char header[HEADER_BYTES], digest[32], *packed = NULL;
	unsigned long packed_size;
	z_stream z;
	int i, result = 1;

	memset(package, 0, sizeof(*package));
	if (!file)
	{
		text_print(error, 256, "can't open %s", path);
		return 1;
	}
	if (fread(header, 1, HEADER_BYTES, file) != HEADER_BYTES || memcmp(header, "OXPK", 4))
	{
		text_print(error, 256, "%s isn't an OpenCE package", path);
		goto done;
	}
	if (u32(header + 4) != FORMAT)
	{
		text_print(error, 256, "%s is a newer package (format %lu): this installer reads format %d", path,
			u32(header + 4), FORMAT);
		goto done;
	}
	packed_size = u32(header + 8);
	package->body_size = u32(header + 12);
	text(package->name, header + 48, 64);
	if (packed_size > 64 * 1024 * 1024 || package->body_size > 64 * 1024 * 1024 || package->body_size < 4)
	{
		text_print(error, 256, "%s's sizes don't read", path);
		goto done;
	}
	packed = malloc(packed_size);
	package->body = malloc(package->body_size);
	if (!packed || !package->body)
	{
		text_print(error, 256, "out of memory");
		goto done;
	}
	if (fread(packed, 1, packed_size, file) != packed_size)
	{
		text_print(error, 256, "%s is cut short", path);
		goto done;
	}
	sha256(packed, packed_size, digest);
	if (memcmp(digest, header + 16, 32))
	{
		text_print(error, 256, "%s is damaged (its contents don't match its SHA-256)", path);
		goto done;
	}
	memset(&z, 0, sizeof(z));
	z.zalloc = z_alloc;
	z.zfree = z_free;
	if (inflateInit(&z) != Z_OK)
	{
		text_print(error, 256, "out of memory");
		goto done;
	}
	z.next_in = packed;
	z.avail_in = packed_size;
	z.next_out = package->body;
	z.avail_out = package->body_size;
	i = inflate(&z, Z_FINISH);
	inflateEnd(&z);
	if (i != Z_STREAM_END || z.avail_out || z.avail_in)
	{
		text_print(error, 256, "%s doesn't unpack", path);
		goto done;
	}
	package->count = (int)u32(package->body);
	if (package->count < 1 || package->count > PACKAGE_MAX_XBES || package->body_size < 4 + 112ul * package->count)
	{
		text_print(error, 256, "%s's table doesn't read", path);
		goto done;
	}
	for (i = 0; i < package->count; i++)
	{
		unsigned char const *t = package->body + 4 + 112 * i;
		struct package_xbe *xbe = &package->xbes[i];
		memcpy(xbe->retail_sha256, t, 32);
		xbe->retail_size = u32(t + 32);
		xbe->build_size = u32(t + 36);
		memcpy(xbe->build_sha256, t + 40, 32);
		xbe->steps = u32(t + 72);
		xbe->steps_size = u32(t + 76);
		text(xbe->name, t + 80, 32);
		if (xbe->steps > package->body_size || xbe->steps_size > package->body_size - xbe->steps ||
			steps_check(package, xbe) != xbe->build_size)
		{
			text_print(error, 256, "%s's steps for %s don't read", path, xbe->name);
			goto done;
		}
	}
	result = 0;
done:
	free(packed);
	if (result)
		package_free(package);
	fclose(file);
	return result;
}

void package_free(struct package *package)
{
	free(package->body);
	package->body = NULL;
	package->count = 0;
}

struct package_xbe const *package_find(struct package const *package, unsigned char const *retail_sha256)
{
	int i;
	for (i = 0; i < package->count; i++)
	{
		if (!memcmp(package->xbes[i].retail_sha256, retail_sha256, 32))
			return &package->xbes[i];
	}
	return NULL;
}

static void put32(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

void package_unpatch(unsigned char *data, struct package_patch const *patch)
{
	if (patch->patched)
		put32(data + patch->cert + 0x9c, patch->media), put32(data + patch->cert + 0xa0, patch->regions);
}

void package_repatch(unsigned char *data, struct package_patch const *patch)
{
	if (patch->patched)
		put32(data + patch->cert + 0x9c, PACKAGE_DISC_MEDIA), put32(data + patch->cert + 0xa0, PACKAGE_DISC_REGIONS);
}

struct package_xbe const *package_match(struct package const *package, unsigned char *data, unsigned long size,
	struct package_patch *patch)
{
	struct package_xbe const *xbe;
	unsigned char digest[32];
	memset(patch, 0, sizeof(*patch));
	sha256(data, size, digest);
	xbe = package_find(package, digest);
	if (xbe || size < 0x200)
		return xbe;
	patch->cert = u32(data + 0x118) - u32(data + 0x104);
	if (patch->cert > size - 0xa4)
		return NULL;
	patch->media = u32(data + patch->cert + 0x9c);
	patch->regions = u32(data + patch->cert + 0xa0);
	if (patch->media == PACKAGE_DISC_MEDIA && patch->regions == PACKAGE_DISC_REGIONS)
		return NULL;
	patch->patched = 1;
	package_repatch(data, patch);
	sha256(data, size, digest);
	xbe = package_find(package, digest);
	if (!xbe)
	{
		package_unpatch(data, patch);
		patch->patched = 0;
	}
	return xbe;
}

/* (the build goes out in parts this big) */
#define CHUNK 65536

int package_make(struct package const *package, struct package_xbe const *xbe, unsigned char const *retail,
	unsigned char const *pictures, unsigned char const *header, package_write_t write, void *context, char *error)
{
	unsigned char const *p = package->body + xbe->steps;
	static unsigned char zeros[CHUNK];
	unsigned char digest[32];
	struct sha256 s;

	sha256_start(&s);
	for (;;)
	{
		int op = *p++;
		unsigned char const *from;
		unsigned long size, at;
		if (op == END)
			break;
		if (op == COPY || op == POOL)
		{
			from = (op == COPY ? retail : package->body) + u32(p);
			size = u32(p + 4);
			p += 8;
			sha256_add(&s, from, size);
		}
		else
		{
			size = u32(p);
			p += 4;
			from = op == PICTURES ? pictures : op == HEADER ? header : NULL;
			sha256_add_zeros(&s, size);
		}
		for (at = 0; at < size; at += CHUNK)
		{
			unsigned long n = size - at < CHUNK ? size - at : CHUNK;
			if (write(context, from ? from + at : zeros, n))
			{
				text_print(error, 256, "couldn't write the new XBE");
				return 1;
			}
		}
	}
	sha256_end(&s, digest);
	if (memcmp(digest, xbe->build_sha256, 32))
	{
		text_print(error, 256, "the XBE made isn't the package's (its SHA-256 differs)");
		return 1;
	}
	return 0;
}

int package_pictures(struct package const *package, struct package_xbe const *xbe, unsigned long *offsets,
	unsigned long *sizes, int most)
{
	unsigned char const *p = package->body + xbe->steps;
	unsigned long made = 0;
	int count = 0;
	for (;;)
	{
		int op = *p++;
		unsigned long size;
		if (op == END)
			return count;
		size = u32(op == COPY || op == POOL ? p + 4 : p);
		p += op == COPY || op == POOL ? 8 : 4;
		if ((op == PICTURES || op == HEADER) && count < most)
		{
			offsets[count] = made;
			sizes[count] = size;
			count++;
		}
		made += size;
	}
}
