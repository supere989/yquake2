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
Machine-readable request, response, and identity contracts are in `schemas/`.
Responses identify the exact tool independently from their parameter-bound
physics identity. The tool identity covers the recursive source/header closure,
all schemas and build support, the compiler and archiver executables and version
output, and the effective compile/link flags. Changing any of those inputs
changes the tool identity; changing a map or Pmove parameter changes the physics
identity as well. External `.ent` entity
fixups are deliberately disabled because these tools expose collision/Pmove,
not entity parsing; the map SHA-256 always identifies the BSP bytes loaded.

## `q2-cm-oracle`

```sh
release/q2-cm-oracle --map /path/to/map.bsp <<'EOF'
{"id":"m","op":"map_info"}
{"id":"c","op":"point_contents","point":[0,0,24]}
{"id":"t","op":"box_trace","start":[0,0,24],"end":[64,0,24],"mins":[-16,-16,-24],"maxs":[16,16,32],"mask":65539}
{"id":"ic","op":"transformed_point_contents","point":[120,50,0],"headnode":42,"origin":[100,50,0],"angles":[0,90,0]}
{"id":"it","op":"transformed_box_trace","start":[50,50,0],"end":[150,50,0],"mins":[0,0,0],"maxs":[0,0,0],"headnode":42,"mask":1,"origin":[100,50,0],"angles":[0,90,0]}
{"id":"v","op":"pvs","from":[0,0,24],"to":[64,0,24]}
EOF
```

Operations are `identity`/`map_info`, `point_contents`, `point_cluster`,
`box_trace`, `transformed_point_contents`, `transformed_box_trace`, `pvs`,
`set_areaportal`, and `areas_connected`. The two transformed operations call
Yamagi's `CM_TransformedPointContents` and `CM_TransformedBoxTrace` directly;
they are the collision authority for translated or rotated BSP brush entities
such as doors, lifts, trains, and buttons. Their `headnode` is mandatory and
must match a headnode from one of the loaded BSP's inline `*N` models. Model 0,
arbitrary tree nodes, and synthetic box headnodes are rejected unless that same
headnode is also owned by a real inline model.

Transformed request coordinates, including trace hull extents, are finite and
bounded to `[-1048576, 1048576]`. Transform angles are degrees in `[-360, 360]`;
callers must normalize equivalent rotations before invoking the oracle. A trace
also rejects any axis where `mins > maxs`. Those limits are part of the request
schema and source-bound tool identity, so malformed, nonfinite, unnormalized,
or out-of-range transforms fail closed instead of reaching collision math.
PVS is explicitly a coarse cluster result; callers must use a `box_trace` or
`transformed_box_trace` with the appropriate mask for occlusion.

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

Consumers should pin an `identity` response and admit subsequent records through
`identity_validation.py`; schema, source closure, build, tool, map, or physics
identity mismatches are rejected. Consumers must fail closed: absent or mismatched collision/Pmove responses
omit the affected Atlas cells or jump/drop edges. They must never substitute a
ballistic or geometry-only approximation.
