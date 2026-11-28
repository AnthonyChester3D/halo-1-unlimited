/*
OXBOX_COLLISION_LAB.C

Lab builds only (-DOXBOX_COLLISION_PROFILE): who calls the collision tests, and what they cost. Every entry point of
the collision code (collisions.c's tests and moves, collision_bsp.c's BSP vector and sphere tests, bsp3d.c's point
lookup) is wrapped where it's defined; only the outermost call is recorded (the tests it makes inside count toward
it), keyed by the test, its caller and its caller's caller (return addresses: tools/collision_lab.py names them),
with calls and processor cycles. Every 10 seconds (oxbox_input.c's memory report) the log gets "collisions 10 s:"
with the ticks run, the cycles spent in all, and the most costly keys.
*/

#if defined(OXBOX_COLLISION_PROFILE)

int snprintf(char *, unsigned int, char const *, ...);
void platform_log(char const *, ...);
long game_time_get(void);

enum { CP_SLOTS = 4096, CP_KINDS = 13, CP_SHOWN = 40 };

static char const *const cp_names[CP_KINDS] = { "test_vector", "features_in_sphere", "test_sphere", "test_point",
	"test_pill", "move_sphere", "move_point", "move_pill", "objects_in_sphere", "fix_pill", "bsp_test_vector",
	"bsp_test_sphere", "bsp3d_test_point" };

static struct cp_slot
{
	unsigned long kind, caller, caller2, calls;
	unsigned long long cycles;
} cp_table[CP_SLOTS];

int oxbox_cp_depth;
/* (inside the features gathered: [0] for a biped's move (collision_move_pill), [1] the rest. Cycles in the level's
BSP sphere test, its features, the objects' loop; objects in the clusters, objects whose features were gathered;
features found in the level; the move through them (cycles, moves); gathers, leaves, radius in thousandths; features
in all) */
unsigned long long oxbox_cp_parts[2][12];
unsigned long long oxbox_cp_bsp[12];   /* (collision_bsp.c: inside the BSP sphere tests) */
int oxbox_cp_in_move;
static unsigned long long cp_kind_cycles[CP_KINDS], cp_window_start;
static unsigned long cp_kind_calls[CP_KINDS], cp_lost;
static long cp_ticks_start;

void oxbox_cp_note(int kind, unsigned long caller, unsigned long caller2, unsigned long long cycles)
{
	unsigned long slot = ((caller * 2654435761u) ^ (caller2 * 40503u) ^ (unsigned long)kind * 97u) & (CP_SLOTS - 1);
	long k;

	cp_kind_cycles[kind] += cycles;
	cp_kind_calls[kind]++;
	for (k = 0; k < CP_SLOTS; k++, slot = (slot + 1) & (CP_SLOTS - 1))
	{
		struct cp_slot *s = &cp_table[slot];

		if (s->calls && s->kind == (unsigned long)kind && s->caller == caller && s->caller2 == caller2)
		{
			s->calls++;
			s->cycles += cycles;
			return;
		}
		if (!s->calls)
		{
			s->kind = (unsigned long)kind;
			s->caller = caller;
			s->caller2 = caller2;
			s->calls = 1;
			s->cycles = cycles;
			return;
		}
	}
	cp_lost++;
}

void oxbox_cp_dump(void)
{
	unsigned long long now = __builtin_ia32_rdtsc(), total = 0;
	long ticks = game_time_get(), k, n;
	char line[900];
	int used;

	for (k = 0; k < CP_KINDS; k++)
		total += cp_kind_cycles[k];
	if (cp_window_start && total)
	{
		used = snprintf(line, sizeof(line), "collisions 10 s: %ld ticks, wall %lu Kc, collision %lu Kc (%lu lost); by test:",
			ticks - cp_ticks_start, (unsigned long)((now - cp_window_start) / 1000), (unsigned long)(total / 1000), cp_lost);
		for (k = 0; k < CP_KINDS; k++)
		{
			if (cp_kind_calls[k] && used < (int)sizeof(line) - 60)
				used += snprintf(line + used, sizeof(line) - used, " %s %lu/%lu Kc", cp_names[k], cp_kind_calls[k],
					(unsigned long)(cp_kind_cycles[k] / 1000));
		}
		platform_log("%s", line);
		for (k = 0; k < 2; k++)
		{
			unsigned long long *q = oxbox_cp_parts[k];

			if (q[8])
				platform_log("collision parts (%s): %lu gathers: bsp sphere %lu Kc, bsp features %lu Kc, objects %lu Kc "
					"(%lu in the clusters, %lu gathered); %lu leaves, radius %lu mm, %lu level features, %lu features in "
					"all a gather on average; the moves: %lu, %lu Kc", k ? "the rest" : "biped moves", (unsigned long)q[8],
					(unsigned long)(q[0] / 1000), (unsigned long)(q[1] / 1000), (unsigned long)(q[2] / 1000),
					(unsigned long)q[3], (unsigned long)q[4], (unsigned long)(q[9] / q[8]), (unsigned long)(q[10] / q[8]),
					(unsigned long)(q[5] / q[8]), (unsigned long)(q[11] / q[8]), (unsigned long)q[7],
					(unsigned long)(q[6] / 1000));
		}
		if (oxbox_cp_bsp[0])
		{
			unsigned long long *b = oxbox_cp_bsp, c = b[0];

			platform_log("collision bsp spheres: %lu calls; a call: %lu.%lu 3D nodes, %lu.%lu leaves, %lu.%lu 2D references "
				"(%lu.%lu walked), %lu.%lu 2D nodes, %lu.%lu surface tests (%lu.%lu again); 2D walks %lu Kc in all, surface "
				"tests %lu Kc", (unsigned long)c, (unsigned long)(b[1] / c), (unsigned long)(b[1] * 10 / c % 10),
				(unsigned long)(b[2] / c), (unsigned long)(b[2] * 10 / c % 10), (unsigned long)(b[3] / c),
				(unsigned long)(b[3] * 10 / c % 10), (unsigned long)(b[4] / c), (unsigned long)(b[4] * 10 / c % 10),
				(unsigned long)(b[5] / c), (unsigned long)(b[5] * 10 / c % 10), (unsigned long)(b[6] / c),
				(unsigned long)(b[6] * 10 / c % 10), (unsigned long)(b[7] / c), (unsigned long)(b[7] * 10 / c % 10),
				(unsigned long)(b[8] / 1000), (unsigned long)(b[9] / 1000));
#if defined(OXBOX_COLLISION_CHECK)
			platform_log("collision check: surfaces passed over that would have added %lu; objects passed over by "
				"the copy: vectors %lu (hit anyway %lu), gathers %lu (features anyway %lu)",
				(unsigned long)(b[10] & 0xFFFF), (unsigned long)(b[11] >> 32), (unsigned long)(b[11] & 0xFFFFFFFF),
				(unsigned long)(b[10] >> 32), (unsigned long)((b[10] >> 16) & 0xFFFF));
#endif
		}
		for (n = 0; n < CP_SHOWN; n++)
		{
			long best = -1;

			for (k = 0; k < CP_SLOTS; k++)
				if (cp_table[k].calls && (best < 0 || cp_table[k].cycles > cp_table[best].cycles))
					best = k;
			if (best < 0)
				break;
			platform_log("collision caller: %s %lx %lx: %lu calls, %lu Kc", cp_names[cp_table[best].kind],
				cp_table[best].caller, cp_table[best].caller2, cp_table[best].calls,
				(unsigned long)(cp_table[best].cycles / 1000));
			cp_table[best].cycles = 0;
			cp_table[best].calls = 0;
		}
	}
	for (k = 0; k < CP_SLOTS; k++)
		cp_table[k].calls = 0;
	for (k = 0; k < CP_KINDS; k++)
	{
		cp_kind_cycles[k] = 0;
		cp_kind_calls[k] = 0;
	}
	cp_lost = 0;
	for (k = 0; k < 2; k++)
		for (n = 0; n < 12; n++)
			oxbox_cp_parts[k][n] = 0;
	for (n = 0; n < 12; n++)
		oxbox_cp_bsp[n] = 0;
	cp_window_start = __builtin_ia32_rdtsc();
	cp_ticks_start = ticks;
}

#endif
