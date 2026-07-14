#!/usr/bin/env python3
"""Generate deterministic source-closure, build, and tool identities."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


COMPILED_SOURCES = (
    "src/common/collision.c",
    "src/common/pmove.c",
    "src/common/shared/shared.c",
    "src/common/crc.c",
    "src/common/md4.c",
    "src/tools/oracle/oracle_support.c",
    "src/tools/oracle/oracle_json.c",
    "src/tools/oracle/sha256.c",
    "src/tools/oracle/q2_cm_oracle.c",
    "src/tools/oracle/q2_pmove_oracle.c",
)
BUILD_INPUTS = (
    "src/tools/oracle/Makefile",
    "src/tools/oracle/gen_identity.py",
    "src/tools/oracle/identity_validation.py",
)
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)


def file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def text_digest(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def c_string(value: str) -> str:
    return (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def command_output(command: list[str]) -> str:
    try:
        result = subprocess.run(
            command, check=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=10,
        )
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"cannot identify toolchain command {command!r}: {error}") from error
    return result.stdout.replace("\r\n", "\n").rstrip("\n")


def executable_identity(command: str, *, compiler: bool) -> dict[str, str]:
    words = shlex.split(command)
    if not words:
        raise SystemExit("empty toolchain command")
    executable = shutil.which(words[0])
    if not executable:
        raise SystemExit(f"toolchain executable not found: {words[0]}")
    executable_path = Path(executable).resolve()
    identity = {
        "command": shlex.join(words),
        "executable_sha256": file_digest(executable_path),
        "version": command_output(words + ["--version"]),
    }
    if compiler:
        identity["target"] = command_output(words + ["-dumpmachine"])
        identity["compiler_version"] = command_output(
            words + ["-dumpfullversion", "-dumpversion"]
        )
    return identity


def resolve_include(root: Path, source: Path, include: str) -> Path | None:
    candidates = (
        source.parent / include,
        root / "src" / include,
        root / "src" / "common" / "header" / include,
        root / "src" / "tools" / "oracle" / include,
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    if include == "oracle_identity.h":
        return None
    raise SystemExit(f"unresolved project include {include!r} from {source}")


def source_closure(root: Path) -> list[Path]:
    pending = [(root / name).resolve() for name in COMPILED_SOURCES + BUILD_INPUTS]
    schema_dir = root / "src" / "tools" / "oracle" / "schemas"
    pending.extend(path.resolve() for path in sorted(schema_dir.glob("*.json")))
    closure: set[Path] = set()
    while pending:
        path = pending.pop()
        if path in closure:
            continue
        if not path.is_file() or root not in path.parents:
            raise SystemExit(f"source closure path is invalid: {path}")
        closure.add(path)
        if path.suffix in {".c", ".h"}:
            for include in INCLUDE.findall(path.read_text(encoding="utf-8")):
                resolved = resolve_include(root, path, include)
                if resolved is not None and resolved not in closure:
                    pending.append(resolved)
    return sorted(closure, key=lambda path: path.relative_to(root).as_posix())


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_identity.py REPOSITORY_ROOT OUTPUT_HEADER")
    root = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2])
    cc = os.environ.get("Q2_ORACLE_CC", "cc")
    ar = os.environ.get("Q2_ORACLE_AR", "ar")
    cflags = os.environ.get("Q2_ORACLE_CFLAGS", "")
    ldflags = os.environ.get("Q2_ORACLE_LDFLAGS", "")

    closure = source_closure(root)
    manifest = [
        {"path": path.relative_to(root).as_posix(), "sha256": file_digest(path)}
        for path in closure
    ]
    manifest_json = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    source_closure_sha256 = text_digest(manifest_json)
    compiler = executable_identity(cc, compiler=True)
    archiver = executable_identity(ar, compiler=False)
    build = {
        "archiver": archiver,
        "cflags": shlex.join(shlex.split(cflags)),
        "compiler": compiler,
        "ldflags": shlex.join(shlex.split(ldflags)),
    }
    build_json = json.dumps(build, sort_keys=True, separators=(",", ":"))
    build_identity_sha256 = text_digest(build_json)
    tool_identity_sha256 = text_digest(json.dumps({
        "schema": "q2-oracle-tool-identity-v1",
        "source_closure_sha256": source_closure_sha256,
        "build_identity_sha256": build_identity_sha256,
    }, sort_keys=True, separators=(",", ":")))
    constants = (
        "stop=100,max=300,duck=100,accel=10,water_accel=10,"
        "friction=6,water_friction=1,water_speed=400,step=18,"
        "clip_planes=5,min_step_normal=0.7"
    )
    body = f"""#ifndef Q2_ORACLE_IDENTITY_H
#define Q2_ORACLE_IDENTITY_H
#define Q2_COLLISION_SOURCE_SHA256 \"{file_digest(root / 'src/common/collision.c')}\"
#define Q2_PMOVE_SOURCE_SHA256 \"{file_digest(root / 'src/common/pmove.c')}\"
#define Q2_SHARED_HEADER_SHA256 \"{file_digest(root / 'src/common/header/shared.h')}\"
#define Q2_SHARED_SOURCE_SHA256 \"{file_digest(root / 'src/common/shared/shared.c')}\"
#define Q2_SOURCE_CLOSURE_SHA256 \"{source_closure_sha256}\"
#define Q2_SOURCE_CLOSURE_COUNT {len(manifest)}
#define Q2_BUILD_IDENTITY_SHA256 \"{build_identity_sha256}\"
#define Q2_TOOL_IDENTITY_SHA256 \"{tool_identity_sha256}\"
#define Q2_COMPILER_COMMAND \"{c_string(compiler['command'])}\"
#define Q2_COMPILER_VERSION \"{c_string(compiler['version'])}\"
#define Q2_COMPILER_TARGET \"{c_string(compiler['target'])}\"
#define Q2_COMPILER_BINARY_SHA256 \"{compiler['executable_sha256']}\"
#define Q2_ARCHIVER_COMMAND \"{c_string(archiver['command'])}\"
#define Q2_ARCHIVER_VERSION \"{c_string(archiver['version'])}\"
#define Q2_ARCHIVER_BINARY_SHA256 \"{archiver['executable_sha256']}\"
#define Q2_EFFECTIVE_CFLAGS \"{c_string(build['cflags'])}\"
#define Q2_EFFECTIVE_LDFLAGS \"{c_string(build['ldflags'])}\"
#define Q2_PMOVE_CONSTANTS \"{c_string(constants)}\"
#endif
"""
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text(encoding="utf-8") != body:
        output.write_text(body, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
