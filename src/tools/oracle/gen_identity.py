#!/usr/bin/env python3
"""Generate reproducible source identities for the offline physics tools."""

from __future__ import annotations

import hashlib
from pathlib import Path
import sys


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def c_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_identity.py REPOSITORY_ROOT OUTPUT_HEADER")
    root = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2])
    collision = digest(root / "src/common/collision.c")
    pmove = digest(root / "src/common/pmove.c")
    shared = digest(root / "src/common/header/shared.h")
    shared_impl = digest(root / "src/common/shared/shared.c")
    build = "oracle-c11-dedicated-f32-wrapv-no-strict-alias-v1"
    constants = (
        "stop=100,max=300,duck=100,accel=10,water_accel=10,"
        "friction=6,water_friction=1,water_speed=400,step=18,"
        "clip_planes=5,min_step_normal=0.7"
    )
    body = f"""#ifndef Q2_ORACLE_IDENTITY_H
#define Q2_ORACLE_IDENTITY_H
#define Q2_COLLISION_SOURCE_SHA256 \"{collision}\"
#define Q2_PMOVE_SOURCE_SHA256 \"{pmove}\"
#define Q2_SHARED_HEADER_SHA256 \"{shared}\"
#define Q2_SHARED_SOURCE_SHA256 \"{shared_impl}\"
#define Q2_ORACLE_BUILD_CONTRACT \"{c_string(build)}\"
#define Q2_PMOVE_CONSTANTS \"{c_string(constants)}\"
#endif
"""
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text(encoding="utf-8") != body:
        output.write_text(body, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
