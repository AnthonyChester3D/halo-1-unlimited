# What a map may carry

What Halo 1 Unlimited accepts in an Xbox map beyond what the retail maps hold, on every build (the Xbox game, the PC game and
the Linux dedicated server).

## Shared files

A map may draw its textures and sounds from shared files: see [shared_maps.md](shared_maps.md).

## A trimmed script node table

A scenario's script syntax data (`hs_syntax_data`, the "script node" data array) may be trimmed to the nodes it uses plus some
spare: the same data array with only its `maximum_count` lowered, anything from its `count` up to the retail 19,001, and the
data's size `0x38 + maximum_count × 0x14` (the array's header, then that many 20-byte nodes). The nodes in use keep their
indices; `count`, `actual_count` and `first_free_absolute_index` stay at or under `maximum_count`. The retail table is always
19,001 nodes (371 KB); most maps use a few thousand, so trimming gives 200-350 KB of tag memory back. Halo Map Porter writes
it.

Leave a few hundred spare nodes: the PC console compiles its expressions into the same table, and a map's scripts may add a
few as they're readied. When the table is full, adding a node fails as it does at 19,001: the expression or script that
needed it doesn't compile (a script the map can't ready doesn't run), and nothing is written past the table. A table whose
header doesn't fit these rules is taken as damaged: the map plays without its scripts (`source/hs/hs.c`,
`hs_scenario_syntax_data_valid`; on the PC and the server also `port/linux/game/tag_schema_scenario.c`).

## MCC's script functions (v1.149)

Maps converted from Halo: The Master Chief Collection's mods (Halo Map Porter) may call these, by MCC's names and
arguments. They're at fixed places in the script function table, the same on the Xbox and the dedicated server
(source/hs/hs.c, `mcc_definitions`, one evaluator `hs_mcc_evaluate`), and maps may call them:

| index | function | returns |
|---|---|---|
| 422 | `bit_test <long> <short bit>` | boolean (false for a bit outside 0-31) |
| 423 | `bit_toggle <long> <short bit> <boolean on>` | long (the bit set or cleared; unchanged outside 0-31) |
| 424 | `bitwise_flags_toggle <long> <long flags> <boolean on>` | long |
| 425-427 | `bitwise_and` / `bitwise_or` / `bitwise_xor <long> <long>` | long |
| 428 | `bitwise_left_shift <long> <short count>` | long (0 for 32 or more) |
| 429 | `bitwise_right_shift <long> <short count>` | long (arithmetic: a negative stays negative) |
| 430 | `objects_distance_to_object <object_list> <object>` | real: the nearest listed object's centre to the object's; -1 when none |
| 431 | `objects_distance_to_flag <object_list> <cutscene_flag>` | real, as above, to the flag |
| 432 | `objects_distance_to_position <object_list> <real x> <real y> <real z>` | real, as above |
| 433 | `list_count_not_dead <object_list>` | short (objects not dead; anything not a unit counts) |
| 434 | `local_players` | object_list: this machine's players' units |
| 435 | `players_on_multiplayer_team <short team>` | object_list: the units of the players on that team (the player's team index: red 0 and blue 1 in team games; a campaign's players are team 1) |
| 436 | `game_is_authoritative` | boolean: false only on a network game's client |
| 437 | `physics_set_gravity <real>` | gravity as a fraction of Halo's (0 to 10); every map starts at 1 |
| 438 | `physics_get_gravity` | real |
| 439 | `physics_constants_reset` | gravity back to 1 |
| 440 | `script_started <script>` | boolean: its thread has begun (false for a dormant script not woken) |
| 441 | `script_finished <script>` | boolean: a startup or dormant script ran to its end |

Gravity is the host's: in a network game, clients predict their own movement at Halo's gravity, so a changed gravity
shows as jumps corrected by the host. Not saved in checkpoints. Converted maps don't need these for MCC's other
functions that Halo has under another name (Halo Map Porter rewrites those: `pin` as `min`/`max`, `sleep_forever` as
`sleep -1`, `breadcrumbs_*_flag/_object` as the team nav point functions...).
