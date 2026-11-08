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
