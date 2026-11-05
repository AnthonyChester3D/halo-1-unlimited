# What the Xbox build's log says (oxbox_log.txt)

Beside the XBE, written as the game runs. Lines worth looking for, from port/oxbox/oxbox_diagnostics.c, game_state.c
and oxbox_entry.c:

| Line | When | Cost when nothing happens |
|---|---|---|
| `limit: <pool> is full (<max>): first at <map>, BSP n, (x y z), t s` | the first time in a map a game state pool (actors, objects, particles, script threads...) or the sound or texture cache is full | none: runs only when a pool is already full |
| `limit: <pool> was full N times in that map` | as the next map starts | none |
| `bookmark N: <map>, BSP n, (x y z), t s, fps, KB free` | both thumbsticks clicked together ("Bookmark N" on the HUD) | a look at two buttons a frame |
| `checkpoint: ...` | each checkpoint saved, each revert, and maps that save none (they lend that memory to the textures) | none |
| `crash: exception <code> at <address>`, registers, `crash: stack ...` | a crash (the exception handler around main) | none |
| `halt: stack ...` | the game halting on an error | none |
| `pools: <pool> used/max ...` | every 10 s of a map, with opence.ini `log_pools = 1` | off: one test a frame; on: 1.5 KB and a walk of the pools every 10 s |

Memory: the code and its tables take about 5 KB of the image (measured in xemu on Precipice: 644 KB free in v1.89,
640 KB in v1.90). The per-frame work with everything off is about 30 instructions.

Crash and halt addresses are the game's (and, among them, some of its data's): look them up in the oxbox.elf of the
same build, e.g. `llvm-symbolizer --obj=oxbox.elf 0x00102a96`. The first `crash: stack` addresses are the nearest
callers.
