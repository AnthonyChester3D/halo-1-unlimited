/*
OBJECT_BOUNDS_CACHE.C

The objects' bounding spheres packed by absolute index, so collision's object
loop (collisions.c) passes over the far objects of a crowded cluster without
reading each one. Written by their only writer (object_compute_node_matrices);
an entry counts only for its datum index and until the game state is next
replaced (game_state.c). The same test on the same values: the features
gathered do not change. (Kept out of the game state, whose layout the saved
games share.) v1.214: the Xbox has it too, and collision's vector test passes
over objects whose copy the vector misses.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "objects/objects.h"

#include <string.h>

#if defined(HALO_XBOX_TARGET)
/* oxbox (v1.214): the copy lives in the room the AI's pools leave on a map that doesn't need it (cache_files.c, after
the host's damage checks: object_bounds_cache_memory), 20 bytes an object (40 KB); none when there's no room (a
campaign level's AI), and every object is read as before. No epoch: invalidating fills the entries with NONE */
struct object_bounds
{
	long object_index;
	real_point3d center;
	real radius;
};

static struct object_bounds *object_bounds;

void object_bounds_cache_invalidate(
	void)
{
	if (object_bounds)
		memset(object_bounds, 0xFF, MAXIMUM_OBJECTS_PER_MAP * sizeof(struct object_bounds));
}

void object_bounds_cache_memory(
	void **idle,
	long *idle_size)
{
	long size = (long)((MAXIMUM_OBJECTS_PER_MAP * sizeof(struct object_bounds) + 0xFFF) & ~0xFFFUL);

	object_bounds = NULL;
	if (idle && *idle && *idle_size >= size)
	{
		object_bounds = (struct object_bounds *)*idle;
		*idle = (byte *)*idle + size;
		*idle_size -= size;
		object_bounds_cache_invalidate();
	}
}

void object_bounds_cache_update(
	long object_index,
	real_point3d const *center,
	real radius)
{
	struct object_bounds *bounds;

	if (!object_bounds)
		return;
	bounds = &object_bounds[DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index)];
	bounds->object_index = object_index;
	bounds->center = *center;
	bounds->radius = radius;
}

boolean object_bounds_cache_out_of_reach(
	long object_index,
	real_point3d const *point,
	real radius)
{
	struct object_bounds const *bounds;

	if (!object_bounds)
		return FALSE;
	bounds = &object_bounds[DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index)];
	return bounds->object_index == object_index && !point_in_sphere(point, &bounds->center, bounds->radius + radius);
}

/* whether the vector is known to miss the object's bounding sphere (object_test_vector's own first test, on the
copy); FALSE when it is not known */
boolean object_bounds_cache_misses_vector(
	long object_index,
	real_point3d const *point,
	real_vector3d const *vector)
{
	struct object_bounds const *bounds;

	if (!object_bounds)
		return FALSE;
	bounds = &object_bounds[DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index)];
	return bounds->object_index == object_index &&
		!fast_vector_intersects_sphere(point, vector, &bounds->center, bounds->radius);
}
#else
struct object_bounds
{
	long object_index;
	long epoch;
	real_point3d center;
	real radius;
};

static struct object_bounds object_bounds[MAXIMUM_OBJECTS_PER_MAP];
static long object_bounds_epoch = 1;

void object_bounds_cache_update(
	long object_index,
	real_point3d const *center,
	real radius)
{
	struct object_bounds *bounds = &object_bounds[DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index)];

	bounds->object_index = object_index;
	bounds->epoch = object_bounds_epoch;
	bounds->center = *center;
	bounds->radius = radius;
}

void object_bounds_cache_invalidate(
	void)
{
	object_bounds_epoch++;
}

/* whether the object's bounding sphere is known to be out of the reach of
the point (collision's own test on the copy); FALSE when it is not known */
boolean object_bounds_cache_out_of_reach(
	long object_index,
	real_point3d const *point,
	real radius)
{
	struct object_bounds const *bounds = &object_bounds[DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index)];

	return bounds->object_index == object_index && bounds->epoch == object_bounds_epoch &&
		!point_in_sphere(point, &bounds->center, bounds->radius + radius);
}

/* whether the vector is known to miss the object's bounding sphere (object_test_vector's own first test, on the
copy); FALSE when it is not known */
boolean object_bounds_cache_misses_vector(
	long object_index,
	real_point3d const *point,
	real_vector3d const *vector)
{
	struct object_bounds const *bounds = &object_bounds[DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index)];

	return bounds->object_index == object_index && bounds->epoch == object_bounds_epoch &&
		!fast_vector_intersects_sphere(point, vector, &bounds->center, bounds->radius);
}
#endif
