# Exact Yamagi collision and movement oracles

These offline-only tools compile the pinned `src/common/collision.c` and
`src/common/pmove.c` unchanged into `release/libq2oracle.a`. They do not enter
the normal client, dedicated-server, or game-module targets.

Build with:

```sh
make -C src/tools/oracle
```

Both CLIs load one IBSP-38 map and process newline-delimited JSON from stdin.
Every request may carry an opaque string `id`; each response echoes it. Invalid
requests return `{"ok":false,...}` and engine map-load failures exit nonzero.
One input line is limited to 1 MiB and a movement request to 4096 commands.
Machine-readable request contracts are in `schemas/`. External `.ent` entity
fixups are deliberately disabled because these tools expose collision/Pmove,
not entity parsing; the map SHA-256 always identifies the BSP bytes loaded.

## `q2-cm-oracle`

```sh
release/q2-cm-oracle --map /path/to/map.bsp <<'EOF'
{"id":"m","op":"map_info"}
{"id":"c","op":"point_contents","point":[0,0,24]}
{"id":"t","op":"box_trace","start":[0,0,24],"end":[64,0,24],"mins":[-16,-16,-24],"maxs":[16,16,32],"mask":65539}
{"id":"v","op":"pvs","from":[0,0,24],"to":[64,0,24]}
EOF
```

Operations are `identity`/`map_info`, `point_contents`, `point_cluster`,
`box_trace`, `pvs`, `set_areaportal`, and `areas_connected`. PVS is explicitly
a coarse cluster result; callers must use a `box_trace` with the appropriate
mask for occlusion.

## `q2-pmove-oracle`

`simulate` accepts world-unit `origin` and `velocity`, a `pmove_state_t`
parameter block, and an ordered `commands` array. Each command is one exact
Yamagi `Pmove` call. Angles can be degrees in `angles` or exact protocol shorts
in `angles_short`.

```json
{"id":"jump","op":"simulate","origin":[0,0,24],"velocity":[0,0,0],"pm_flags":4,"gravity":800,"airaccelerate":0,"commands":[{"msec":100,"angles":[0,0,0],"forwardmove":300,"upmove":320}]}
```

Responses include every fixed-point state and its world-unit projection,
ground/water/duck state, view geometry, the map digest, source digests, frozen
movement constants, and a physics identity bound to the map, gravity, and air
acceleration. Atlas manifests must reject identity mismatches.

Consumers must fail closed: absent or mismatched collision/Pmove responses
omit the affected Atlas cells or jump/drop edges. They must never substitute a
ballistic or geometry-only approximation.
