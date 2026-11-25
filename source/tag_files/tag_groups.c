/*
TAG_GROUPS.C
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "tag_files.h"
#include "byte_swapping.h"
#include "tag_groups.h"
#include "custom_edition_cache.h" /* port: port/linux/game/custom_edition_cache.c */

/* ---------- constants */

enum
{
	/* port: the most bytes of the empty data (tag_empty_data): more than
	any tag's root or any block's element */
#if defined(OXBOX_STRIP)
	/* oxbox: 16 KB, four times the largest tag root (the dialogue's, 4,112
	bytes; the scenario's 1,456) or block element */
	TAG_EMPTY_DATA_SIZE = 0x4000,
#else
	TAG_EMPTY_DATA_SIZE = 0x10000,
#endif
};

/* ---------- globals */

/* port: (tag_empty_data) */
static unsigned long tag_empty_data_bytes[TAG_EMPTY_DATA_SIZE / sizeof(unsigned long)];

/* ---------- private code */

/* port: an index past what it indexes, logged once */
static void tag_index_error(
	char const *what,
	long index,
	long count)
{
	static boolean logged = FALSE;

	if (!logged)
	{
		logged = TRUE;
		error(_error_silent, "#%ld is not a %s index in [#0,#%ld): an empty one is used", index, what, count);
	}

	return;
}

/* ---------- public code */

/* port: what an index into a tag block, a tag's data or the tags that is
not one gives (tag_block_get_element_with_size, tag_data_get_pointer,
tag_get): TAG_EMPTY_DATA_SIZE bytes of zeros, zeroed again each time, in
place of whatever lies past the block, the data or the tags. Whatever
reads it reads an element or tag with nothing in it (no elements in its
blocks, no tags referenced, every index 0); whatever writes it writes
nowhere that matters */
static long tag_empty_data_given = TAG_EMPTY_DATA_SIZE;

void *tag_empty_data(
	void)
{
	csmemset(tag_empty_data_bytes, 0, sizeof(tag_empty_data_bytes));
	tag_empty_data_given = TAG_EMPTY_DATA_SIZE;   /* (v1.187: all of it given: the next sized caller zeroes all) */

	return tag_empty_data_bytes;
}

/* oxbox (v1.187): the empty data for a caller that asked for size bytes (an element or a tag's data): only those
zeroed again. A map that asks every frame for an element that isn't there (Boxfight's T4d Flood Room: an index of -1
into an empty block, many times a frame) had all 16 KB zeroed each time, ~2 MB a frame: memset was 60% of the
processor there (console profile). Zeroed: what this caller asked for and what the one before was given (what it
may have written), so the buffer stays zeros but for the last caller's part, as when all of it was zeroed each time */
static void *tag_empty_data_sized(
	long size)
{
	long zero = size > tag_empty_data_given ? size : tag_empty_data_given;

	if (zero > 0)
		csmemset(tag_empty_data_bytes, 0, (unsigned long)zero);
	tag_empty_data_given = size > 0 ? size : 0;

	return tag_empty_data_bytes;
}

/* port: (cache_files.c) */
boolean tag_index_is_group(long tag_index, long group_tag);

long verify_tag_reference(
	const struct tag_reference *reference)
{
	long index;

	match_assert("c:\\halo\\SOURCE\\tag_files\\tag_groups.c", 3055, reference);
	/* port: a protected Custom Edition map has its tag names replaced and
	its references' names emptied, so a reference is taken by its index
	(port/linux/game/custom_edition_cache.c) */
	if (custom_edition_cache_tags_loaded())
		return tag_index_is_group(reference->index, reference->group_tag) ? reference->index : NONE;
	index = tag_loaded(reference->group_tag, reference->name);
	
	match_vassert(
		"c:\\halo\\SOURCE\\tag_files\\tag_groups.c", 3061, reference->index==index,
		csprintf(temporary,
			"tag reference \"%s\" and actual index do not match: is %08lX but should be %08lX",
			reference->name,
			reference->index,
			index));

	return index;
}

void* tag_data_get_pointer(
	const struct tag_data *data,
	long offset, 
	long size) 
{
	/* port: Halo PC reads a Custom Edition map's tags unchecked, and maps
	made for it can hold an offset past a tag data's end, which never
	stopped a game there: it gets the empty data below without an
	assertion. This build's maps keep theirs */
	if (!custom_edition_cache_tags_loaded())
	{
		match_assert("c:\\halo\\SOURCE\\tag_files\\tag_groups.c", 3073, size>=0);
		match_assert("c:\\halo\\SOURCE\\tag_files\\tag_groups.c", 3074, offset>=0 && offset+size<=data->size);
	}
	/* port: bytes past the data are the empty data's (tag_empty_data), as
	far as they go */
	if (size < 0 || offset < 0 || offset > data->size || size > data->size - offset || (size && !data->address))
	{
		tag_index_error("data", offset, data->size);
		return size <= TAG_EMPTY_DATA_SIZE ? tag_empty_data_sized(size) : NULL;
	}

	return (void *)((byte *)data->address + offset);
}

void *tag_block_get_element_with_size(
	const struct tag_block *block,
	long index, 
	long element_size) 
{
	match_assert("c:\\halo\\SOURCE\\tag_files\\tag_groups.c", 3084, block);
	match_assert("c:\\halo\\SOURCE\\tag_files\\tag_groups.c", 3085, block->count>=0);
	match_assert("c:\\halo\\SOURCE\\tag_files\\tag_groups.c", 3086, !block->definition || block->definition->element_size==element_size);

	/* port: as tag_data_get_pointer, a Custom Edition map's index past a
	block's end (which Halo PC never checked: foundation@ce, 13 seconds in)
	gets the empty data below without an assertion. This build's maps keep
	theirs */
	if (!custom_edition_cache_tags_loaded())
	{
		match_vassert("c:\\halo\\SOURCE\\tag_files\\tag_groups.c", 3089, index>=0 && index<block->count,
			csprintf(temporary,
				"#%d is not a valid %s index in [#0,#%d)",
				index,
				block->definition ? block->definition->name : "<unknown>", block->count));
		match_assert("c:\\halo\\SOURCE\\tag_files\\tag_groups.c", 3090, block->address);
	}
	/* port: an element past the block (an index a map's data gave, which
	nothing checked) is the empty data (tag_empty_data), not whatever lies
	past the block */
	if (index < 0 || index >= block->count || !block->address)
	{
#if defined(HALO_XBOX_TARGET)
		/* oxbox (v1.189): each code site asking for an element that isn't there, said once (oxbox_log.txt) with the
		index, the block's element count and size: the address names the code (build's oxbox.elf), which tells what
		a map's data lacks (v1.188's T4d Flood Room: element #0 of an empty block, ~11 a second in combat, which
		debug.txt's single line couldn't place). Lab builds also count them (OXBOX_CALL_COUNT) */
		{
			extern void platform_log(char const *format, ...);
			static unsigned long said[64];
			unsigned long caller = (unsigned long)__builtin_return_address(0);
			int k;

#if defined(OXBOX_CALL_COUNT)
			{
				extern void oxbox_count_add(int kind, unsigned long caller, unsigned long amount);

				oxbox_count_add(6, caller, 1);
			}
#endif
			for (k = 0; k < 64 && said[k] && said[k] != caller; k++)
				;
			if (k < 64 && !said[k])
			{
				said[k] = caller;
				platform_log("tags: %lx asked for element #%ld of %s (%ld elements, %ld bytes each)", caller, index,
					block->definition ? block->definition->name : "<no definition>", block->count, element_size);
			}
		}
#endif
		tag_index_error("block element", index, block->count);
		return element_size <= TAG_EMPTY_DATA_SIZE ? tag_empty_data_sized(element_size) : NULL;
	}

	return (void *)((byte *)block->address + (index * element_size));
}
