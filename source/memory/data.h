/*
DATA.H

header included in hcex build.
*/

#ifndef __DATA_H
#define __DATA_H
#pragma once

/* ---------- headers */

#include "tag_files.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

struct data_array
{
	char name[TAG_STRING_LENGTH+1];
	short maximum_count;
	short size;
	boolean valid;
	boolean identifier_zero_invalid;
	unsigned long signature;
	short first_free_absolute_index;
	short count;
	short actual_count;
	short next_identifier;
	void *data;
};

struct datum_header
{
	short identifier;
};

struct data_iterator
{
	struct data_array *data;
	short absolute_index;
	short pad;
	long datum_index;
	unsigned long signature;
};

typedef char data_iterator_size_assert[
	sizeof(struct data_iterator) == 0x10 ? 1 : -1];

/* ---------- prototypes/DATA.C */

long data_allocation_size(short maximum_count, short size);
void data_initialize(struct data_array *data, const char *name, short maximum_count, short size);
void *datum_try_and_get(struct data_array *data, long index);
void data_verify(struct data_array *data);
void *datum_get(struct data_array *data, long index);
#if defined(HALO_XBOX_TARGET)
/* oxbox (v1.137): datum_get's answer when everything checks out, else NULL (datum_get then says why): datum_get's own
fast path, and the hottest callers' (objects.c's object_get_and_verify_type), without a call or a stack frame */
static __inline__ void *datum_get_fast(
	struct data_array *data,
	long index)
{
	short identifier = (short)(index >> 16), absolute_index = (short)index;
	struct datum_header *header;

	if (!data->valid || (!identifier && data->identifier_zero_invalid) || absolute_index < 0 ||
		absolute_index >= data->count)
		return NULL;
	header = (struct datum_header *)((char *)data->data + data->size * absolute_index);
	return header->identifier && (!identifier || identifier == header->identifier) ? header : NULL;
}
#endif
#if defined(HALO_XBOX_TARGET) && !defined(OXBOX_NO_FAST_LOOKUPS)
/* oxbox (v1.185): datum_get with its usual case inline, for the busiest callers (the cluster partitions' walks as
objects move and the lights' per-cluster lists: ~45% of datum_get's calls in a game with bots, xemu count) */
static __inline__ void *datum_get_inline(
	struct data_array *data,
	long index)
{
	void *header = datum_get_fast(data, index);

	return header ? header : datum_get(data, index);
}
#else
#define datum_get_inline datum_get
#endif
struct data_array *data_new(const char *name, short maximum_count, short size);
void data_dispose(struct data_array *data);
void data_make_invalid(struct data_array *data);
long datum_new_at_index(struct data_array *data, long index);
long datum_new(struct data_array *data);
void datum_delete(struct data_array *data, long index);
void data_delete_all(struct data_array *data);
void data_iterator_new(struct data_iterator *iterator, struct data_array *data);
void *data_iterator_next(struct data_iterator *iterator);
long data_next_index(struct data_array *data, long index);
long data_prev_index(struct data_array *data, long index);
void data_compact(struct data_array *data);
void data_make_valid(struct data_array *data);


/* ---------- globals */

/* ---------- public code */

#endif // __DATA_H
