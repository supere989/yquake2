"""Small deterministic Quake II BSP v38 fixtures for collision/Pmove parity tests."""

from __future__ import annotations

import struct
from pathlib import Path

HEADER_LUMPS = 19
LUMP_ENTITIES = 0
LUMP_PLANES = 1
LUMP_VISIBILITY = 3
LUMP_NODES = 4
LUMP_TEXINFO = 5
LUMP_LEAFS = 8
LUMP_LEAFBRUSHES = 10
LUMP_MODELS = 13
LUMP_BRUSHES = 14
LUMP_BRUSHSIDES = 15
LUMP_AREAS = 17
LUMP_AREAPORTALS = 18

CONTENTS_SOLID = 1


def _plane(normal: tuple[float, float, float], distance: float) -> bytes:
    # collision.c's axial node fast path assumes the positive normal. Mark
    # negative axial planes as PLANE_ANY* so its dot-product path preserves
    # the orientation used by this compact generated BSP.
    axial_type = next((i if value == 1 else i + 3 for i, value in enumerate(normal) if abs(value) == 1), 3)
    return struct.pack("<4fi", *normal, distance, axial_type)


def _leaf(contents: int, cluster: int, first_brush: int, brush_count: int) -> bytes:
    return struct.pack(
        "<ihh3h3h4H",
        contents,
        cluster,
        0,
        -4096,
        -4096,
        -4096,
        4096,
        4096,
        4096,
        0,
        0,
        first_brush,
        brush_count,
    )


def _visibility(clusters: int) -> bytes:
    row_bytes = (clusters + 7) // 8
    header_bytes = 4 + clusters * 8
    rows = []
    offsets = []
    all_visible = bytes([0xFF] * row_bytes)
    for index in range(clusters):
        offset = header_bytes + index * row_bytes
        offsets.append((offset, offset))
        rows.append(all_visible)
    return struct.pack("<i", clusters) + b"".join(
        struct.pack("<2i", *entry) for entry in offsets
    ) + b"".join(rows)


def write_bsp(
    path: Path,
    *,
    brushes: list[tuple[tuple[float, float, float], tuple[float, float, float], int]] | None = None,
    split_contents: int | None = None,
) -> None:
    """Write a valid collision-only BSP.

    Trace maps put every supplied AABB brush in the single empty world leaf.
    Contents maps split at x=0: x>=0 is empty and x<0 has split_contents.
    """
    brushes = list(brushes or [])
    # collision.c requires at least one leafbrush; this unreachable brush is a
    # structural sentinel when the caller only needs point contents/PVS.
    if not brushes:
        brushes.append(((3000.0, 3000.0, 3000.0), (3010.0, 3010.0, 3010.0), CONTENTS_SOLID))

    planes = [_plane((1.0, 0.0, 0.0), 0.0), _plane((-1.0, 0.0, 0.0), 0.0)]
    brush_records = []
    brush_sides = []
    brush_plane_numbers = []
    for mins, maxs, contents in brushes:
        first_side = len(brush_sides)
        sides = (
            ((-1.0, 0.0, 0.0), -mins[0]),
            ((1.0, 0.0, 0.0), maxs[0]),
            ((0.0, -1.0, 0.0), -mins[1]),
            ((0.0, 1.0, 0.0), maxs[1]),
            ((0.0, 0.0, -1.0), -mins[2]),
            ((0.0, 0.0, 1.0), maxs[2]),
        )
        plane_numbers = []
        for normal, distance in sides:
            plane_number = len(planes)
            planes.append(_plane(normal, distance))
            brush_sides.append(struct.pack("<Hh", plane_number, 0))
            plane_numbers.append(plane_number)
        brush_plane_numbers.append(plane_numbers)
        brush_records.append(struct.pack("<3i", first_side, 6, contents))

    leafbrushes = b"".join(struct.pack("<H", i) for i in range(len(brushes)))
    if split_contents is None:
        # A six-node chain per convex brush. Front of any outward plane is
        # outside and advances to the next brush; back of all six reaches the
        # brush leaf. This is a small but spatially correct BSP union.
        nodes = []
        for brush_index, plane_numbers in enumerate(brush_plane_numbers):
            next_brush = (brush_index + 1) * 6 if brush_index + 1 < len(brushes) else -2
            inside_leaf = -(brush_index + 3)
            for side_index, plane_number in enumerate(plane_numbers):
                back = brush_index * 6 + side_index + 1 if side_index < 5 else inside_leaf
                nodes.append((plane_number, next_brush, back))
        leaves = _leaf(CONTENTS_SOLID, -1, 0, 0) + _leaf(0, 0, 0, 0)
        for brush_index, (_, _, contents) in enumerate(brushes):
            leaves += _leaf(contents, -1, brush_index, 1)
        clusters = 1
    else:
        nodes = [(0, -2, -3)]
        leaves = (
            _leaf(CONTENTS_SOLID, -1, 0, 0)
            + _leaf(0, 0, 0, 0)
            + _leaf(split_contents, 1, 0, 0)
        )
        clusters = 2

    node = b"".join(
        struct.pack(
            "<i2i3h3h2H", plane, front, back,
            -4096, -4096, -4096, 4096, 4096, 4096, 0, 0,
        )
        for plane, front, back in nodes
    )
    texinfo = struct.pack(
        "<8fii32si", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, b"oracle\0", -1
    )
    model = struct.pack(
        "<9f3i",
        -4096,
        -4096,
        -4096,
        4096,
        4096,
        4096,
        0,
        0,
        0,
        0,
        0,
        0,
    )
    lumps = [b"" for _ in range(HEADER_LUMPS)]
    lumps[LUMP_ENTITIES] = b'{"classname" "worldspawn"}\0'
    lumps[LUMP_PLANES] = b"".join(planes)
    lumps[LUMP_VISIBILITY] = _visibility(clusters)
    lumps[LUMP_NODES] = node
    lumps[LUMP_TEXINFO] = texinfo
    lumps[LUMP_LEAFS] = leaves
    lumps[LUMP_LEAFBRUSHES] = leafbrushes
    lumps[LUMP_MODELS] = model
    lumps[LUMP_BRUSHES] = b"".join(brush_records)
    lumps[LUMP_BRUSHSIDES] = b"".join(brush_sides)
    lumps[LUMP_AREAS] = struct.pack("<2i", 0, 0)
    lumps[LUMP_AREAPORTALS] = b""

    header_size = 8 + HEADER_LUMPS * 8
    offset = header_size
    directory = []
    body = bytearray()
    for lump in lumps:
        padding = (-offset) & 3
        if padding:
            body.extend(b"\0" * padding)
            offset += padding
        directory.append((offset, len(lump)))
        body.extend(lump)
        offset += len(lump)
    header = struct.pack("<4sI", b"IBSP", 38) + b"".join(
        struct.pack("<2i", *entry) for entry in directory
    )
    path.write_bytes(header + body)
