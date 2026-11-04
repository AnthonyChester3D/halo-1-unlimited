# Memory measuring pass (v1.85, November 2026)

What the game state's areas reserve and what a busy match actually uses. Measured in xemu on the
`OXBOX_TEST_POOLS` test build (each data array's live entries noted every frame, the session's peak
logged), hosting a 16-player Slayer match with bots (Suggested: the host and 15 bots) for about two
minutes after loading:

- **Sidewinder**: the biggest stock map, no AI.
- **Precipice**: a big ported Custom Edition map that uses AI (its marines).

xemu's frame rate isn't a console's, but these counts are about what's in the world, not speed.

## Game state: reserved and used

"Peak" is the most entries alive at once in that session.

| Area | Room | Reserved | Peak, Sidewinder | Peak, Precipice |
|---|---|---|---|---|
| objects (memory pool) | 1024 KB | 1024 KB | 235 KB | 478 KB |
| hs thread (script threads) | 256 x 536 B | 134 KB | 1 | 22 |
| actor | 64 x 1828 B | 114 KB | 0 | 9 |
| particle | 1024 x 112 B | 112 KB | 932 | 672 |
| decals | 2048 x 56 B | 112 KB | 1116 | 957 |
| lights | 896 x 124 B | 108 KB | 125 | 358 |
| particle system particles | 512 x 128 B | 64 KB | 280 | 78 |
| cached object render states | 256 x 256 B | 64 KB | 172 | 204 |
| effect | 256 x 252 B | 63 KB | 16 | 42 |
| prop (AI's knowledge of units) | 192 x 312 B | 58 KB | 0 | 168 |
| contrail point | 1024 x 56 B | 56 KB | 453 | 146 |
| object looping sounds | 1024 x 52 B | 52 KB | 11 | 102 |
| glow particles | 512 x 100 B | 50 KB | 0 | - |
| effect location | 512 x 60 B | 30 KB | 33 | 100 |
| object / cluster reference lists (8 lists) | 2048 x 12 B each | 24 KB each | 33-340 | 158-445 |
| particle systems | 64 x 344 B | 21 KB | 8 | 7 |
| contrail | 256 x 68 B | 17 KB | 20 | 15 |
| encounter | 128 x 108 B | 13 KB | 0 | 7 |
| flag | 2 x 5820 B | 11 KB | 0 | - |
| ai pursuit | 256 x 40 B | 10 KB | 0 | 1 |
| swarm component, swarm | 128 x 64 B, 32 x 152 B | 12 KB | 0 | 26, 8 |
| device groups | 1024 x 8 B | 8 KB | 0 | 182 |
| hs globals | 1024 x 8 B | 8 KB | 443 | 456 |

On Sidewinder, **295 KB** of data arrays held nothing at all for the whole match. 207 KB of that is
the AI: actors 114 KB, props 58 KB, encounters 13 KB, swarms 12 KB and pursuits 10 KB.

## What it says

- **Close to full: particles.** Sidewinder peaked at 932 of 1024 with 16 players. When it's full, new
  particles just don't appear (a visual loss, not a crash). Worth watching on effect-heavy ported
  maps.
- **Close to full: props on an AI map with 16 players.** Precipice peaked at 168 of 192. The AI's
  knowledge grows with the number of players, so a busier AI map could run out. Then actors would
  stop noticing some players. Raising the room to 256 would cost about 20 KB of game state.
- **Mostly empty: script threads.** 134 KB reserved, 22 used on the busiest map here. Halving the room
  to 128 would give back 67 KB on every console. But a Custom Edition map or campaign co-op with many
  scripts could need more, so it needs measuring on script-heavy maps first.
- **Unused in most matches: the campaign AI.** 207 KB is idle on any map without encounters, which is
  most multiplayer maps.

## Lending the AI's memory to the bots' waypoints: not done

The first idea for borrowing was to put the bots' waypoint network in the actors' room on maps
without AI. It isn't done. The game resets a data array at every new game (`data_delete_all` clears
the first two bytes of each of its 64 entries), which would cut into a network kept between games of
the same map. Lending it safely would need the network rebuilt at every new game, and that's worth
testing on a console first.

It also matters less since v1.85 moved the network itself (see below). Waypoints now take memory only
on a host that has bots, sized to the map:

- Battle Creek: 145 waypoints, 14 KB.
- Sidewinder: 215 waypoints, 20 KB.
- Precipice: 232 waypoints, 22 KB.

Before v1.85, every Xbox had about 85 KB set aside for them, bots or not.

## Freed in v1.85

- **About 85 KB on every Xbox:** the bots' waypoints (512 x 80 B), the search queue (33 KB) and four
  sets of search arrays are now heap memory, taken only by a host with bots and freed when it stops
  hosting.

## Running it again

Build with `-DOXBOX_TEST_POOLS` added to `OXBOX_EXTRA`. Each 10 seconds of a game, the log gets
`pools:` lines (the session's peaks). Two minutes in, it adds `pools unused:` lines listing the
reserved room nothing has used yet, biggest first.
