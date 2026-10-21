/* oxbox_memprof.c: who holds the Xbox's 64 MB.

The retail XBE's kernel imports (the thunk table at the start of .rdata, which the loader filled with the kernel's
function addresses) are pointed at wrappers here for the kernel's allocators: contiguous memory (XPhysicalAlloc,
Direct3D's frame buffers and push buffer, DirectSound), system memory, virtual memory (the XAPI heaps) and the
pool (XNet, the kernel's own objects). Each live allocation is kept with its callers (return addresses into the
image found on the stack), and oxbox_memprof_report() logs the kernel's page statistics (MmQueryStatistics) and the
live allocations grouped by caller, largest first; port/oxbox/tools/memprof.py names the addresses. */
#include <xtl.h>
#include <string.h>

#if defined(OXBOX_MEMPROF)

void platform_log(const char *format, ...);
void *oxbox_kernel_export(unsigned long ordinal);
extern char __oxbox_image_end[];

enum { KIND_CONTIGUOUS, KIND_SYSTEM, KIND_VIRTUAL, KIND_POOL, NUMBER_OF_KINDS };
static const char *const kind_names[NUMBER_OF_KINDS] = { "contiguous", "system", "virtual", "pool" };

enum { MAXIMUM_LIVE = 1536, CALLERS = 4 };
static struct live
{
	unsigned long base, size;
	unsigned long callers[CALLERS];
	unsigned char kind;
} live[MAXIMUM_LIVE];
static long live_count;
/* (set from a debugger: the next memory line reports, oxbox_input.c) */
volatile int oxbox_memprof_request;
static unsigned long untracked;   /* (allocations past the table) */

typedef void * (WINAPI *contiguous_proc)(unsigned long size);
typedef void * (WINAPI *contiguous_ex_proc)(unsigned long size, unsigned long low, unsigned long high, unsigned long alignment, unsigned long protect);
typedef void (WINAPI *free_contiguous_proc)(void * base);
typedef void * (WINAPI *system_proc)(unsigned long size, unsigned long protect);
typedef unsigned long (WINAPI *free_system_proc)(void * base, unsigned long size);
typedef LONG (WINAPI *virtual_proc)(void * *base, unsigned long zero_bits, unsigned long * size, unsigned long type, unsigned long protect);
typedef LONG (WINAPI *free_virtual_proc)(void * *base, unsigned long * size, unsigned long type);
typedef void * (WINAPI *pool_proc)(unsigned long size);
typedef void * (WINAPI *pool_tag_proc)(unsigned long size, unsigned long tag);
typedef void (WINAPI *free_pool_proc)(void * base);
typedef unsigned long (WINAPI *query_size_proc)(void * base);
typedef unsigned long (WINAPI *set_protect_proc)(void * base, unsigned long size, unsigned long protect);

static contiguous_proc real_contiguous;
static contiguous_ex_proc real_contiguous_ex;
static free_contiguous_proc real_free_contiguous;
static system_proc real_system;
static free_system_proc real_free_system;
static virtual_proc real_virtual;
static free_virtual_proc real_free_virtual;
static pool_proc real_pool;
static pool_tag_proc real_pool_tag;
static free_pool_proc real_free_pool;
static query_size_proc query_size;

/* live allocations by base: open addressing (EMPTY, DEAD or a live index), so a free is not a walk of the table
(the pool is allocated and freed per packet and per sound) */
enum { SLOTS = 4096, EMPTY = -1, DEAD = -2 };
static short slots[SLOTS];
static long dead_slots;
static int slots_ready;

static unsigned long slot_hash(unsigned long base)
{
	return ((base >> 4) * 2654435761UL) >> 20;
}

static void slot_insert(unsigned long base, long index)
{
	unsigned long slot = slot_hash(base);

	while (slots[slot] >= 0)
		slot = (slot + 1) & (SLOTS - 1);
	if (slots[slot] == DEAD)
		dead_slots--;
	slots[slot] = (short)index;
}

/* the slot of the live allocation at base of the kind, or -1 */
static long slot_find(unsigned long base, int kind)
{
	unsigned long slot = slot_hash(base);
	long probes;

	for (probes = 0; probes < SLOTS && slots[slot] != EMPTY; probes++, slot = (slot + 1) & (SLOTS - 1))
	{
		if (slots[slot] >= 0 && live[slots[slot]].base == base && live[slots[slot]].kind == kind)
			return (long)slot;
	}
	return -1;
}

static void slots_rebuild(void)
{
	long index;

	for (index = 0; index < SLOTS; index++)
		slots[index] = EMPTY;
	dead_slots = 0;
	for (index = 0; index < live_count; index++)
		slot_insert(live[index].base, index);
	slots_ready = 1;
}

/* whether the address is a return address into the image: just after a call */
static int return_address(unsigned long address)
{
	const unsigned char *p = (const unsigned char *)address;

	if (address < 0x11000 || address >= (unsigned long)__oxbox_image_end)
		return 0;
	return p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) ||
		(p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10);
}

/* (noinline: its frame is the wrapper's caller's) */
static __attribute__((noinline)) void note(unsigned long base, unsigned long size, int kind)
{
	unsigned long const *stack = (unsigned long const *)__builtin_frame_address(0);
	struct live *entry;
	int found = 0, index;

	if (!base || !size)
		return;
	if (live_count >= MAXIMUM_LIVE)
	{
		untracked += size;
		return;
	}
	slot_insert(base, live_count);
	entry = &live[live_count++];
	memset(entry, 0, sizeof(*entry));
	entry->base = base;
	entry->size = size;
	entry->kind = (unsigned char)kind;
	for (index = 0; index < 128 && found < CALLERS; index++)
	{
		if (return_address(stack[index]))
			entry->callers[found++] = stack[index];
	}
}

/* whether two allocations have the same kind and callers (not memcmp: clang makes it bcmp, which isn't here) */
static int same_group(struct live const *a, struct live const *b)
{
	return a->kind == b->kind && a->callers[0] == b->callers[0] && a->callers[1] == b->callers[1] &&
		a->callers[2] == b->callers[2] && a->callers[3] == b->callers[3];
}

/* the live allocation at base, forgotten; its size (0: not one of ours) */
static unsigned long forget(unsigned long base, int kind)
{
	long slot = slot_find(base, kind);
	long index, last;
	unsigned long size;

	if (slot < 0)
		return 0;
	index = slots[slot];
	size = live[index].size;
	slots[slot] = DEAD;
	dead_slots++;
	last = --live_count;
	if (index != last)
	{
		/* (the last entry moves into the hole: its slot follows it) */
		long moved = slot_find(live[last].base, live[last].kind);

		live[index] = live[last];
		if (moved >= 0)
			slots[moved] = (short)index;
	}
	if (dead_slots > SLOTS / 4)
		slots_rebuild();
	return size;
}

static void * WINAPI hook_contiguous(unsigned long size)
{
	void * result = real_contiguous(size);
	note((unsigned long)result, (size + 4095) & ~4095UL, KIND_CONTIGUOUS);
	return result;
}

static void * WINAPI hook_contiguous_ex(unsigned long size, unsigned long low, unsigned long high, unsigned long alignment, unsigned long protect)
{
	void * result = real_contiguous_ex(size, low, high, alignment, protect);
	note((unsigned long)result, (size + 4095) & ~4095UL, KIND_CONTIGUOUS);
	return result;
}

static void WINAPI hook_free_contiguous(void * base)
{
	forget((unsigned long)base, KIND_CONTIGUOUS);
	real_free_contiguous(base);
}

static void * WINAPI hook_system(unsigned long size, unsigned long protect)
{
	void * result = real_system(size, protect);
	note((unsigned long)result, (size + 4095) & ~4095UL, KIND_SYSTEM);
	return result;
}

static unsigned long WINAPI hook_free_system(void * base, unsigned long size)
{
	forget((unsigned long)base, KIND_SYSTEM);
	return real_free_system(base, size);
}

/* virtual memory: what is committed (MEM_COMMIT 0x1000), by region; a decommit or release forgets the region at
that base (the XAPI heaps commit and decommit inside their reservations: approximate) */
static LONG WINAPI hook_virtual(void * *base, unsigned long zero_bits, unsigned long * size, unsigned long type, unsigned long protect)
{
	LONG result = real_virtual(base, zero_bits, size, type, protect);
	if (result >= 0 && (type & 0x1000) && base && size)
	{
		forget((unsigned long)*base, KIND_VIRTUAL);
		note((unsigned long)*base, (unsigned long)*size, KIND_VIRTUAL);
	}
	return result;
}

static LONG WINAPI hook_free_virtual(void * *base, unsigned long * size, unsigned long type)
{
	unsigned long at = base ? (unsigned long)*base : 0;
	LONG result = real_free_virtual(base, size, type);
	if (result >= 0 && at)
		forget(at, KIND_VIRTUAL);
	return result;
}

static void * WINAPI hook_pool(unsigned long size)
{
	void * result = real_pool(size);
	if (size >= 1024)
		note((unsigned long)result, size, KIND_POOL);
	return result;
}

static void * WINAPI hook_pool_tag(unsigned long size, unsigned long tag)
{
	void * result = real_pool_tag(size, tag);
	if (size >= 1024)
		note((unsigned long)result, size, KIND_POOL);
	return result;
}

static void WINAPI hook_free_pool(void * base)
{
	forget((unsigned long)base, KIND_POOL);
	real_free_pool(base);
}

static struct
{
	unsigned long ordinal;
	void *hook;
	void **real;
} const hooks[] = {
	{ 165, (void *)hook_contiguous, (void **)&real_contiguous },
	{ 166, (void *)hook_contiguous_ex, (void **)&real_contiguous_ex },
	{ 171, (void *)hook_free_contiguous, (void **)&real_free_contiguous },
	{ 167, (void *)hook_system, (void **)&real_system },
	{ 172, (void *)hook_free_system, (void **)&real_free_system },
	{ 184, (void *)hook_virtual, (void **)&real_virtual },
	{ 199, (void *)hook_free_virtual, (void **)&real_free_virtual },
	{ 14, (void *)hook_pool, (void **)&real_pool },
	{ 15, (void *)hook_pool_tag, (void **)&real_pool_tag },
	{ 17, (void *)hook_free_pool, (void **)&real_free_pool },
};

void oxbox_memprof_initialize(void)
{
	/* (the retail XBE's kernel thunk table, the start of .rdata: the header's copy of its address is not readable
	   once the game runs) */
	unsigned long *thunks = (unsigned long *)0x1d6620;
	set_protect_proc set_protect = (set_protect_proc)oxbox_kernel_export(182);
	unsigned long count, index, hook, hooked = 0;

	slots_rebuild();
	query_size = (query_size_proc)oxbox_kernel_export(180);
	for (count = 0; thunks[count]; count++)
		;
	if (set_protect)
		set_protect(thunks, count * 4, PAGE_READWRITE);
	for (hook = 0; hook < sizeof(hooks) / sizeof(hooks[0]); hook++)
	{
		void *function = oxbox_kernel_export(hooks[hook].ordinal);

		*hooks[hook].real = function;
		for (index = 0; function && index < count; index++)
		{
			if (thunks[index] == (unsigned long)function)
			{
				thunks[index] = (unsigned long)hooks[hook].hook;
				hooked++;
			}
		}
	}
	platform_log("memprof: %lu of the %lu kernel imports watched", hooked, count);
}

/* MmQueryStatistics' record */
struct mm_statistics
{
	unsigned long length, total_pages, available_pages, virtual_bytes_committed, virtual_bytes_reserved,
		cache_pages, pool_pages, stack_pages, image_pages;
};

void oxbox_memprof_report(const char *label)
{
	typedef LONG (WINAPI *statistics_proc)(struct mm_statistics *);
	statistics_proc statistics = (statistics_proc)oxbox_kernel_export(181);
	struct mm_statistics mm;
	unsigned long totals[NUMBER_OF_KINDS] = { 0 };
	static unsigned char reported[MAXIMUM_LIVE];
	long index, other;

	memset(&mm, 0, sizeof(mm));
	mm.length = sizeof(mm);
	if (statistics)
		statistics(&mm);
	for (index = 0; index < live_count; index++)
		totals[live[index].kind] += live[index].size;
	platform_log("memprof %s: pages (4 KB) total %lu, available %lu, image %lu, stacks %lu, pool %lu, file cache %lu, "
		"virtual committed %lu KB", label, mm.total_pages, mm.available_pages, mm.image_pages, mm.stack_pages,
		mm.pool_pages, mm.cache_pages, mm.virtual_bytes_committed / 1024);
	platform_log("memprof %s: watched: contiguous %lu KB, system %lu KB, virtual %lu KB, pool %lu KB, untracked %lu KB "
		"(%ld live)", label, totals[0] / 1024, totals[1] / 1024, totals[2] / 1024, totals[3] / 1024, untracked / 1024,
		live_count);
	/* grouped by kind and callers, largest first */
	memset(reported, 0, sizeof(reported));
	for (;;)
	{
		long best = -1;
		unsigned long best_size = 0, count = 0;

		for (index = 0; index < live_count; index++)
		{
			unsigned long size = 0;
			long other_index;

			if (reported[index])
				continue;
			for (other_index = index; other_index < live_count; other_index++)
			{
				if (!reported[other_index] && same_group(&live[other_index], &live[index]))
				{
					size += live[other_index].size;
				}
			}
			if (size > best_size)
			{
				best_size = size;
				best = index;
			}
		}
		if (best < 0)
			break;
		for (other = best; other < live_count; other++)
		{
			if (!reported[other] && same_group(&live[other], &live[best]))
			{
				reported[other] = 1;
				count++;
			}
		}
		if (best_size >= 8192)
		{
			platform_log("memprof %s: %7lu KB %-10s x%-3lu by %08lx %08lx %08lx %08lx", label, best_size / 1024,
				kind_names[live[best].kind], count, live[best].callers[0], live[best].callers[1], live[best].callers[2], live[best].callers[3]);
		}
	}
}

#endif
