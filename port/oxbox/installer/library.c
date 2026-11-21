/* library.c: the map library (library.h, docs/map_library.md).

The catalog: "OXLB", u32 format (1), u32 the text's size, the text (ASCII "key: value" lines: the library's, then an
item's from each "item:" line), then the signatures as a package's ("OXSG", u32 count, then each key's id and an
Ed25519 signature of "Halo 1 Unlimited map library, signed\n" + the 12 bytes before the text + the text).

A map: its zip downloaded beside its folder (<folder>.hmp_dl.zip), its SHA-256 and size checked as it comes, unpacked
into <folder>.hmp_dl\ (each file's CRC-32 checked, nothing outside the folder), the mark written in it, then swapped
in: the folder there now renamed <folder>.hmp_old, the new one renamed <folder>, the old one deleted. The game skips
any folder with ".hmp_" in its name, so a power cut never shows half a map; library_tidy puts an old folder back if
the swap was cut between its renames, and deletes the rest. */
#include "library.h"
#include "package.h"
#include "sha256.h"
#include "text.h"
#include "monocypher-ed25519.h"

#include <windows.h>
#include <zlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library_keys.h"

#define MOST_CATALOG (4ul << 20)
#define MOST_ITEMS 4000
#define SIGNED_PREFIX "Halo 1 Unlimited map library, signed\n"
#define PIECE (256ul << 10)
#define MOST_NAME 84

/* ---- the catalog ---- */

struct grow
{
	unsigned char *data;
	unsigned long size;
};

static int grow_put(void *context, unsigned char const *data, unsigned long size)
{
	struct grow *g = context;
	if (g->size + size > MOST_CATALOG)
		return 1;
	memcpy(g->data + g->size, data, size);
	g->size += size;
	return 0;
}

static unsigned long u32(unsigned char const *p)
{
	return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

static unsigned short u16(unsigned char const *p)
{
	return (unsigned short)(p[0] | p[1] << 8);
}

/* a value copied (at most size - 1 characters) */
static void value_copy(char *to, int size, char const *value, int length)
{
	if (length > size - 1)
		length = size - 1;
	memcpy(to, value, length);
	to[length] = 0;
}

static int hex_digest(char const *text, int length, unsigned char *digest)
{
	int k;
	if (length != 64)
		return 0;
	for (k = 0; k < 64; k++)
	{
		char c = text[k];
		int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (v < 0)
			return 0;
		digest[k / 2] = (unsigned char)(k & 1 ? digest[k / 2] | v : v << 4);
	}
	return 1;
}

/* an item's folder name: what the game reads and FATX allows (and no ".hmp_", which the game skips) */
static int folder_ok(char const *name, int pack)
{
	char const *p;
	size_t n = strlen(name);
	if (!n || n > (pack ? 30 : LIBRARY_MAP_NAME) || name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.' || !strcmp(name, ".."))
		return 0;
	for (p = name; *p; p++)
		if (*p < 32 || *p > 126 || strchr("<>:\"/\\|?*", *p))
			return 0;
	for (p = name; *p; p++)
		if (!_strnicmp(p, ".hmp_", 5))
			return 0;
	return 1;
}

static int parse(char *text, unsigned long size, struct library *library, char *error)
{
	char *line = text, *end = text + size;
	struct library_item *item = NULL;
	int count = 0;
	for (; line < end; line++)
		count += !strncmp(line, "item: ", 6) && (line == text || line[-1] == '\n');
	if (count > MOST_ITEMS)
		count = MOST_ITEMS;
	library->items = calloc(count ? count : 1, sizeof(*library->items));
	if (!library->items)
	{
		text_print(error, 256, "not enough memory for the library's list");
		return 1;
	}
	for (line = text; line < end;)
	{
		char *next = memchr(line, '\n', end - line), *colon, *value;
		int key_length, length;
		if (!next)
			next = end;
		colon = memchr(line, ':', next - line);
		if (colon)
		{
			key_length = (int)(colon - line);
			for (value = colon + 1; value < next && *value == ' '; value++)
				;
			length = (int)(next - value);
			while (length && (value[length - 1] == '\r' || value[length - 1] == ' '))
				length--;
			if (key_length == 4 && !strncmp(line, "item", 4))
			{
				item = library->count < count ? &library->items[library->count++] : NULL;
				if (item)
					value_copy(item->item, sizeof(item->item), value, length);
			}
			else if (!item)
			{
				if (key_length == 6 && !strncmp(line, "serial", 6))
					library->serial = strtoul(value, NULL, 10);
				else if (key_length == 6 && !strncmp(line, "format", 6) && strtoul(value, NULL, 10) != 1)
				{
					text_print(error, 256, "the library's list is a newer kind: update the installer first");
					return 1;
				}
			}
			else if (key_length == 4 && !strncmp(line, "name", 4))
				value_copy(item->name, sizeof(item->name), value, length);
			else if (key_length == 5 && !strncmp(line, "group", 5))
				value_copy(item->group, sizeof(item->group), value, length);
			else if (key_length == 7 && !strncmp(line, "version", 7))
				value_copy(item->version, sizeof(item->version), value, length);
			else if (key_length == 5 && !strncmp(line, "about", 5))
				value_copy(item->about, sizeof(item->about), value, length);
			else if (key_length == 4 && !strncmp(line, "file", 4))
				value_copy(item->file, sizeof(item->file), value, length);
			else if (key_length == 4 && !strncmp(line, "size", 4))
				item->size = strtoul(value, NULL, 10);
			else if (key_length == 8 && !strncmp(line, "unpacked", 8))
				item->unpacked = strtoul(value, NULL, 10);
			else if (key_length == 4 && !strncmp(line, "kind", 4))
				item->pack = length == 4 && !strncmp(value, "pack", 4);
			else if (key_length == 5 && !strncmp(line, "files", 5))
				item->files = strtoul(value, NULL, 10);
			else if (key_length == 6 && !strncmp(line, "sha256", 6) && !hex_digest(value, length, item->sha256))
				item->file[0] = 0;
		}
		line = next + 1;
	}
	/* (an item that can't be put on an Xbox is dropped: a bad folder name, no file, no size) */
	{
		int i, n = 0;
		for (i = 0; i < library->count; i++)
		{
			struct library_item *it = &library->items[i];
			if (folder_ok(it->item, it->pack) && it->file[0] && it->size && !strstr(it->file, "..") && it->unpacked < 0xF0000000ul)
			{
				if (!it->name[0])
					text_print(it->name, sizeof(it->name), "%s", it->item);
				library->items[n++] = *it;
			}
		}
		library->count = n;
	}
	return 0;
}

int library_ready(void)
{
	return library_key_count > 0;   /* (none: mkkeys.py writes one empty entry and a count of 0) */
}

int library_fetch(char const *url, int allow_unsigned, struct library *library, char *error)
{
	struct grow g = {NULL, 0};
	unsigned long size, text_size, at, count, k;
	unsigned char *signed_part = NULL;
	int i, good = 0;
	char const *slash;
	memset(library, 0, sizeof(*library));
	slash = strrchr(url, '/');
	value_copy(library->base, sizeof(library->base), url, slash ? (int)(slash - url) + 1 : 0);
	g.data = malloc(MOST_CATALOG);
	if (!g.data)
	{
		text_print(error, 256, "not enough memory for the library's list");
		return 1;
	}
	if (download(url, MOST_CATALOG, grow_put, &g, NULL, &size, error))
	{
		free(g.data);
		return 1;
	}
	error[0] = 0;
	if (g.size < 12 || memcmp(g.data, "OXLB", 4) || (text_size = u32(g.data + 8)) > g.size - 12)
	{
		text_print(error, 256, "what came back isn't the library's list");
		free(g.data);
		return 2;
	}
	/* (signed by a key in library_keys.txt) */
	at = 12 + text_size;
	if (g.size >= at + 8 && !memcmp(g.data + at, "OXSG", 4) && (count = u32(g.data + at + 4)) <= 8 &&
		g.size == at + 8 + count * 72)
	{
		unsigned long prefix = sizeof(SIGNED_PREFIX) - 1, signed_size = prefix + at;
		signed_part = malloc(signed_size);
		if (signed_part)
		{
			memcpy(signed_part, SIGNED_PREFIX, prefix);
			memcpy(signed_part + prefix, g.data, at);
			for (k = 0; k < count && !good; k++)
			{
				unsigned char const *s = g.data + at + 8 + k * 72;
				for (i = 0; i < library_key_count && !good; i++)
				{
					if (library_keys[i].name[0] && !memcmp(library_keys[i].id, s, 8) &&
						!crypto_ed25519_check(s + 8, library_keys[i].key, signed_part, signed_size))
					{
						good = 1;
						text_print(library->signer, sizeof(library->signer), "%s", library_keys[i].name);
					}
				}
			}
			free(signed_part);
		}
	}
	if (!good && !allow_unsigned)
	{
		text_print(error, 256, "the library's list isn't signed by the library's key");
		free(g.data);
		return 2;
	}
	g.data[at] = 0;
	i = parse((char *)g.data + 12, text_size, library, error);
	free(g.data);
	if (i)
		library_free(library);
	return i ? 2 : 0;
}

void library_free(struct library *library)
{
	free(library->items);
	library->items = NULL;
	library->count = 0;
}

/* ---- on the Xbox ---- */

/* (nxdk's zlib is Z_SOLO: no allocator of its own) */
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

static int folder_exists(char const *path)
{
	DWORD a = GetFileAttributesA(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* the mark in a folder: its sha256 and group, 0 if it has none */
static int mark_read(char const *folder, unsigned char *digest, char *group, int group_size)
{
	char path[300], line[128];
	FILE *file;
	int found = 0;
	text_print(path, sizeof(path), "%s\\" LIBRARY_MARK, folder);
	group[0] = 0;
	if (!(file = fopen(path, "r")))
		return 0;
	while (fgets(line, sizeof(line), file))
	{
		int n = (int)strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		if (!strncmp(line, "sha256: ", 8))
			found = hex_digest(line + 8, n - 8, digest);
		else if (!strncmp(line, "group: ", 7))
			text_print(group, group_size, "%s", line + 7);
	}
	fclose(file);
	return found;
}

void library_folder(struct library_item const *item, char const *maps, char *folder, int size)
{
	if (item->pack)
		text_print(folder, size, "%s\\%s", maps, item->item);
	else
		text_print(folder, size, "%s\\" LIBRARY_PACK "\\%s", maps, item->item);
}

void library_states(struct library *library, char const *maps)
{
	int i;
	for (i = 0; i < library->count; i++)
	{
		struct library_item *item = &library->items[i];
		char folder[300], group[32];
		unsigned char digest[32];
		library_folder(item, maps, folder, sizeof(folder));
		if (!folder_exists(folder))
		{
			/* (a copy put in maps\<item>\ by hand: the download takes its place) */
			text_print(folder, sizeof(folder), "%s\\%s", maps, item->item);
			item->state = !item->pack && folder_exists(folder) ? LIBRARY_OTHER : LIBRARY_NEW;
		}
		else if (!mark_read(folder, digest, group, sizeof(group)))
			item->state = LIBRARY_OTHER;
		else if (!memcmp(digest, item->sha256, 32))
			item->state = LIBRARY_INSTALLED;
		else
			item->state = !_stricmp(group, item->group) ? LIBRARY_UPDATE : LIBRARY_SWAP;
	}
}

/* a folder and what's in it (a map's folder: one level of folders in it at most, two to be safe) */
static void delete_tree(char const *folder, int depth)
{
	WIN32_FIND_DATAA found;
	char pattern[300], path[300];
	HANDLE find;
	text_print(pattern, sizeof(pattern), "%s\\*", folder);
	find = FindFirstFileA(pattern, &found);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, ".."))
				continue;
			text_print(path, sizeof(path), "%s\\%s", folder, found.cFileName);
			if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				if (depth > 0)
					delete_tree(path, depth - 1);
			}
			else
				DeleteFileA(path);
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
	RemoveDirectoryA(folder);
}

void library_tidy(char const *maps)
{
	WIN32_FIND_DATAA found;
	char pattern[300], path[300], base[300];
	HANDLE find;
	int pass;
	/* (first an old folder whose new one never got its name back in place, then what's left) */
	for (pass = 0; pass < 2; pass++)
	{
		text_print(pattern, sizeof(pattern), "%s\\*.hmp_*", maps);
		find = FindFirstFileA(pattern, &found);
		if (find == INVALID_HANDLE_VALUE)
			continue;
		do
		{
			char *mark = strstr(found.cFileName, ".hmp_");
			int is_old = mark && !_stricmp(mark, ".hmp_old"), is_dl = mark && (!_stricmp(mark, ".hmp_dl") ||
				!_stricmp(mark, ".hmp_dl.zip"));
			if (!is_old && !is_dl)
				continue;
			text_print(path, sizeof(path), "%s\\%s", maps, found.cFileName);
			text_print(base, sizeof(base), "%s\\%.*s", maps, (int)(mark - found.cFileName), found.cFileName);
			if (pass == 0 && is_old && !folder_exists(base))
				MoveFileA(path, base);
			else if (pass == 1)
			{
				if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					delete_tree(path, 2);
				else
					DeleteFileA(path);
			}
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
}

void library_settle(struct library const *library, char const *maps)
{
	char pack[300], top[300], folder[300], group[32];
	unsigned char digest[32];
	int i;
	text_print(pack, sizeof(pack), "%s\\" LIBRARY_PACK, maps);
	library_tidy(maps);
	library_tidy(pack);
	for (i = 0; i < library->count; i++)
	{
		struct library_item const *item = &library->items[i];
		if (item->pack)
			continue;
		text_print(top, sizeof(top), "%s\\%s", maps, item->item);
		library_folder(item, maps, folder, sizeof(folder));
		if (folder_exists(top) && mark_read(top, digest, group, sizeof(group)) && !folder_exists(folder))
		{
			CreateDirectoryA(pack, NULL);
			MoveFileA(top, folder);
		}
	}
}

struct writer
{
	HANDLE file;
	struct sha256 sha;
	unsigned long done;
	int failed;
};

static int write_put(void *context, unsigned char const *data, unsigned long size)
{
	struct writer *w = context;
	DWORD wrote = 0;
	if (!WriteFile(w->file, data, size, &wrote, NULL) || wrote != size)
	{
		w->failed = 1;
		return 1;
	}
	sha256_add(&w->sha, data, size);
	w->done += size;
	return 0;
}

static int read_at(HANDLE file, unsigned long at, void *buffer, unsigned long size)
{
	LONG high = 0;
	DWORD got = 0;
	if (SetFilePointer(file, (LONG)at, &high, FILE_BEGIN) == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
		return 0;
	return ReadFile(file, buffer, size, &got, NULL) && got == size;
}

/* a name in the zip: a file of the map's folder, or of one folder in it ("x/y"), nothing else */
static int zip_name_ok(char const *name)
{
	char const *slash = strchr(name, '/'), *p;
	size_t n = strlen(name);
	if (!n || n > MOST_NAME || name[0] == '/' || name[n - 1] == '/' || strchr(name, '\\') || strstr(name, ".."))
		return 0;
	if (slash && strchr(slash + 1, '/'))
		return 0;
	for (p = name; *p; p++)
		if (*p < 32 || *p > 126 || strchr("<>:\"|?*", *p))
			return 0;
	return strlen(slash ? slash + 1 : name) <= 42 && (!slash || slash - name <= 42);
}

/* the zip's files into folder (made), each one's CRC-32 checked; total: what the catalog says it unpacks to */
static int unzip(char const *zip, char const *folder, unsigned long total, struct download_report const *report,
	char *error)
{
	HANDLE file = CreateFileA(zip, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	unsigned char tail[22], *directory = NULL, *in = NULL, *out = NULL;
	unsigned long size, entries, directory_size, directory_at, k, at, sum = 0, done = 0;
	int result = 1;
	if (file == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "couldn't open what was downloaded");
		return 1;
	}
	size = GetFileSize(file, NULL);
	/* (the end of the central directory: the zips library.py makes have no comment) */
	if (size < 22 || !read_at(file, size - 22, tail, 22) || u32(tail) != 0x06054b50ul)
	{
		text_print(error, 256, "the download isn't a zip this can read");
		goto done;
	}
	entries = u16(tail + 10);
	directory_size = u32(tail + 12);
	directory_at = u32(tail + 16);
	if (directory_at + directory_size > size - 22 || directory_size > (1ul << 20) ||
		!(directory = malloc(directory_size)) || !(in = malloc(PIECE)) || !(out = malloc(PIECE)) ||
		!read_at(file, directory_at, directory, directory_size))
	{
		text_print(error, 256, "the download's list of files doesn't read");
		goto done;
	}
	if (!CreateDirectoryA(folder, NULL) && !folder_exists(folder))
	{
		text_print(error, 256, "couldn't make a folder in the maps folder");
		goto done;
	}
	for (k = 0, at = 0; k < entries; k++)
	{
		unsigned char const *e = directory + at;
		unsigned char local[30];
		unsigned long method, crc, packed, unpacked, name_length, offset, data_at, left, crc_now = crc32(0, NULL, 0);
		char name[MOST_NAME + 1], path[300], *p;
		HANDLE target;
		z_stream z;
		int inflating = 0, ok = 1;
		if (at + 46 > directory_size || u32(e) != 0x02014b50ul)
			break;
		method = u16(e + 10);
		crc = u32(e + 16);
		packed = u32(e + 20);
		unpacked = u32(e + 24);
		name_length = u16(e + 28);
		offset = u32(e + 42);
		if (at + 46 + name_length > directory_size || name_length > MOST_NAME)
			break;
		memcpy(name, e + 46, name_length);
		name[name_length] = 0;
		at += 46 + name_length + u16(e + 30) + u16(e + 32);
		sum += unpacked;
		if ((u16(e + 8) & 1) || (method != 0 && method != 8) || !zip_name_ok(name) || sum > total ||
			(method == 0 && packed != unpacked))
		{
			text_print(error, 256, "the download has a file this can't take (%.40s)", name);
			goto done;
		}
		if (!read_at(file, offset, local, 30) || u32(local) != 0x04034b50ul)
		{
			text_print(error, 256, "the download is damaged (%.40s)", name);
			goto done;
		}
		data_at = offset + 30 + u16(local + 26) + u16(local + 28);
		if (data_at + packed > directory_at)
		{
			text_print(error, 256, "the download is damaged (%.40s)", name);
			goto done;
		}
		text_print(path, sizeof(path), "%s\\%s", folder, name);
		for (p = path + strlen(folder) + 1; *p; p++)
		{
			if (*p == '/')
			{
				*p = 0;
				CreateDirectoryA(path, NULL);
				*p = '\\';
			}
		}
		target = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (target == INVALID_HANDLE_VALUE)
		{
			text_print(error, 256, "couldn't write %.40s", name);
			goto done;
		}
		if (method == 8)
		{
			memset(&z, 0, sizeof(z));
			z.zalloc = z_alloc;
			z.zfree = z_free;
			inflating = inflateInit2(&z, -15) == Z_OK;
			ok = inflating;
		}
		SetFilePointer(target, 0, NULL, FILE_BEGIN);
		{
			LONG high = 0;
			SetFilePointer(file, (LONG)data_at, &high, FILE_BEGIN);
		}
		for (left = packed; ok && (left || inflating);)
		{
			unsigned long piece = left < PIECE ? left : PIECE;
			DWORD got = 0, wrote = 0;
			if (piece && (!ReadFile(file, in, piece, &got, NULL) || got != piece))
			{
				ok = 0;
				break;
			}
			left -= piece;
			if (!inflating)
			{
				crc_now = crc32(crc_now, in, piece);
				ok = WriteFile(target, in, piece, &wrote, NULL) && wrote == piece;
				done += piece;
			}
			else
			{
				int status;
				z.next_in = in;
				z.avail_in = piece;
				do
				{
					unsigned long made;
					z.next_out = out;
					z.avail_out = PIECE;
					status = inflate(&z, Z_NO_FLUSH);
					made = PIECE - z.avail_out;
					if (status != Z_OK && status != Z_STREAM_END && !(status == Z_BUF_ERROR && !made))
					{
						ok = 0;
						break;
					}
					crc_now = crc32(crc_now, out, made);
					if (made && (!WriteFile(target, out, made, &wrote, NULL) || wrote != made))
						ok = 0;
					done += made;
				} while (ok && status != Z_STREAM_END && (z.avail_in || !z.avail_out));
				if (status == Z_STREAM_END)
				{
					inflateEnd(&z);
					inflating = 0;
					if (left)
						ok = 0;
				}
				else if (!left && !z.avail_in)
					ok = 0;              /* (it ran out before its end) */
			}
			if (report && report->progress)
				report->progress(report->context, done, total);
		}
		if (inflating)
			inflateEnd(&z);
		CloseHandle(target);
		if (!ok || crc_now != crc || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			text_print(error, 256, "%.40s didn't unpack right (the download may be damaged)", name);
			goto done;
		}
	}
	if (k != entries)
		text_print(error, 256, "the download's list of files doesn't read");
	else
		result = 0;
done:
	free(directory);
	free(in);
	free(out);
	CloseHandle(file);
	return result;
}

static int free_megabytes(char const *maps, unsigned long *megabytes)
{
	ULARGE_INTEGER available, total, free_bytes;
	char root[4] = {maps[0], ':', '\\', 0};
	if (!GetDiskFreeSpaceExA(root, &available, &total, &free_bytes))
		return 0;
	*megabytes = (unsigned long)(available.QuadPart >> 20);
	return 1;
}

int library_get(struct library const *library, struct library_item const *item, char const *maps,
	struct download_report const *report, char *error)
{
	char url[720], zip[300], fresh[300], final[300], old[300], mark[320], parent[300], top[300];
	unsigned char digest[32];
	unsigned long size = 0, megabytes, need = (item->size >> 20) + (item->unpacked >> 20) + 2;
	struct writer w;
	FILE *file;
	library_folder(item, maps, final, sizeof(final));
	text_print(parent, sizeof(parent), "%s", final);
	*strrchr(parent, '\\') = 0;
	CreateDirectoryA(parent, NULL);
	library_tidy(parent);
	if (free_megabytes(maps, &megabytes) && megabytes < need)
	{
		text_print(error, 256, "%c: needs %lu MB free for it (%lu MB now)", maps[0], need, megabytes);
		return 1;
	}
	if (!strncmp(item->file, "http://", 7) || !strncmp(item->file, "https://", 8))
		text_print(url, sizeof(url), "%s", item->file);
	else
		text_print(url, sizeof(url), "%s%s", library->base, item->file);
	text_print(zip, sizeof(zip), "%s.hmp_dl.zip", final);
	text_print(fresh, sizeof(fresh), "%s.hmp_dl", final);
	text_print(old, sizeof(old), "%s.hmp_old", final);
	memset(&w, 0, sizeof(w));
	sha256_start(&w.sha);
	w.file = CreateFileA(zip, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (w.file == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "couldn't write in %s", maps);
		return 1;
	}
	if (download(url, item->size, write_put, &w, report, &size, error))
	{
		CloseHandle(w.file);
		DeleteFileA(zip);
		if (w.failed)
			text_print(error, 256, "couldn't write the download (is %c: full?)", maps[0]);
		return 1;
	}
	CloseHandle(w.file);
	sha256_end(&w.sha, digest);
	if (w.done != item->size || memcmp(digest, item->sha256, 32))
	{
		DeleteFileA(zip);
		text_print(error, 256, "the download isn't the library's file (it changed, or came damaged): try again");
		return 1;
	}
	if (unzip(zip, fresh, item->unpacked, report, error))
	{
		DeleteFileA(zip);
		delete_tree(fresh, 2);
		return 1;
	}
	DeleteFileA(zip);
	text_print(mark, sizeof(mark), "%s\\" LIBRARY_MARK, fresh);
	if ((file = fopen(mark, "w")) != NULL)
	{
		int k;
		fprintf(file, "From the map library: %s\nfile: %s\ngroup: %s\nsha256: ", item->name, item->file, item->group);
		for (k = 0; k < 32; k++)
			fprintf(file, "%02x", item->sha256[k]);
		fprintf(file, "\n");
		fclose(file);
	}
	/* (swapped in: the old one aside, the new one in its place, the old one gone) */
	if (folder_exists(final) && !MoveFileA(final, old))
	{
		delete_tree(fresh, 2);
		text_print(error, 256, "couldn't move the map that's there now aside");
		return 1;
	}
	if (!MoveFileA(fresh, final))
	{
		MoveFileA(old, final);
		delete_tree(fresh, 2);
		text_print(error, 256, "couldn't put the map in place");
		return 1;
	}
	if (folder_exists(old))
		delete_tree(old, 2);
	/* (a copy put in maps\<item>\ by hand: this one in its place now) */
	text_print(top, sizeof(top), "%s\\%s", maps, item->item);
	if (!item->pack && folder_exists(top))
		delete_tree(top, 2);
	return 0;
}

int library_remove(struct library_item const *item, char const *maps, char *error)
{
	char folder[300];
	library_folder(item, maps, folder, sizeof(folder));
	delete_tree(folder, 2);
	if (folder_exists(folder))
	{
		text_print(error, 256, "couldn't delete all of %s", folder);
		return 1;
	}
	/* (and the Library folder, once nothing's in it) */
	if (!item->pack)
	{
		*strrchr(folder, '\\') = 0;
		RemoveDirectoryA(folder);
	}
	return 0;
}
