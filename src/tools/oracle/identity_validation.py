#!/usr/bin/env python3
"""Fail-closed validation for Q2 oracle responses and admission identities."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


SHA256 = re.compile(r"^[0-9a-f]{64}$")
COMMON = {
    "ok", "id", "op", "schema", "tool_identity", "physics_identity",
    "map_sha256", "map_checksum",
}
CM_FIELDS = {
    "identity": {"provenance", "source", "map", "model0", "clusters", "inline_models"},
    "map_info": {"provenance", "source", "map", "model0", "clusters", "inline_models"},
    "point_contents": {"point", "headnode", "contents"},
    "point_cluster": {"point", "leaf", "cluster", "area", "contents"},
    "box_trace": {
        "headnode", "mask", "fraction", "allsolid", "startsolid", "endpos",
        "plane", "contents", "surface",
    },
    "pvs": {"from_cluster", "to_cluster", "potentially_visible"},
    "set_areaportal": {"portal", "open"},
    "areas_connected": {"area1", "area2", "connected"},
}
PMOVE_FIELDS = {
    "identity": {"parameters", "provenance", "source"},
    "simulate": {"frames", "final", "command_count"},
}
TOOL_KEYS = {
    "schema", "tool_identity", "source_closure_sha256", "source_closure_count",
    "build_identity_sha256", "compiler", "archiver", "build",
}


class IdentityMismatch(ValueError):
    """The response cannot be admitted under the expected oracle identity."""


def _reject(condition: bool, message: str) -> None:
    if condition:
        raise IdentityMismatch(message)


def _exact_keys(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    _reject(not isinstance(value, dict), f"{label} must be an object")
    actual = set(value)
    _reject(actual != keys, f"{label} fields differ: missing={sorted(keys - actual)}, unknown={sorted(actual - keys)}")
    return value


def _sha(value: Any, label: str) -> None:
    _reject(not isinstance(value, str) or SHA256.fullmatch(value) is None,
            f"{label} is not a lowercase SHA-256")


def validate_tool_provenance(value: Any) -> dict[str, Any]:
    provenance = _exact_keys(value, TOOL_KEYS, "provenance")
    _reject(provenance["schema"] != "q2-oracle-tool-identity-v1", "tool schema mismatch")
    for name in ("tool_identity", "source_closure_sha256", "build_identity_sha256"):
        _sha(provenance[name], f"provenance.{name}")
    _reject(type(provenance["source_closure_count"]) is not int or
            provenance["source_closure_count"] < 1, "invalid source closure count")
    compiler = _exact_keys(provenance["compiler"],
                           {"command", "version", "target", "executable_sha256"}, "compiler")
    archiver = _exact_keys(provenance["archiver"],
                           {"command", "version", "executable_sha256"}, "archiver")
    build = _exact_keys(provenance["build"], {"cflags", "ldflags"}, "build")
    for label, tool, strings in (
        ("compiler", compiler, ("command", "version", "target")),
        ("archiver", archiver, ("command", "version")),
    ):
        for name in strings:
            _reject(not isinstance(tool[name], str) or not tool[name], f"{label}.{name} is empty")
        _sha(tool["executable_sha256"], f"{label}.executable_sha256")
    for name in ("cflags", "ldflags"):
        _reject(not isinstance(build[name], str), f"build.{name} must be a string")
    return provenance


def _validate_source(source: Any, kind: str) -> None:
    keys = {"collision_sha256", "shared_header_sha256", "shared_source_sha256"}
    if kind == "pmove":
        keys.add("pmove_sha256")
    source = _exact_keys(source, keys, "source")
    for name, digest in source.items():
        _sha(digest, f"source.{name}")


def validate_identity_record(record: Any, kind: str) -> dict[str, Any]:
    """Validate the exact identity response shape without admitting it yet."""
    fields = CM_FIELDS if kind == "cm" else PMOVE_FIELDS if kind == "pmove" else None
    _reject(fields is None, f"unsupported oracle kind {kind!r}")
    _reject(not isinstance(record, dict) or record.get("ok") is not True, "identity response is not successful")
    _reject(record.get("op") != "identity", "response is not an identity operation")
    _exact_keys(record, COMMON | fields["identity"], "identity response")
    _reject(record["schema"] != f"q2-{kind}-oracle-v1", "response schema mismatch")
    for name in ("tool_identity", "physics_identity", "map_sha256"):
        _sha(record[name], name)
    _reject(type(record["map_checksum"]) is not int or record["map_checksum"] < 0,
            "invalid map checksum")
    provenance = validate_tool_provenance(record["provenance"])
    _reject(provenance["tool_identity"] != record["tool_identity"],
            "response/provenance tool identity mismatch")
    _validate_source(record["source"], kind)
    return record


def validate_response(record: Any, expected_identity: dict[str, Any], kind: str) -> dict[str, Any]:
    """Validate and admit one response against a previously pinned identity record."""
    expected = validate_identity_record(expected_identity, kind)
    fields = CM_FIELDS if kind == "cm" else PMOVE_FIELDS
    _reject(not isinstance(record, dict) or record.get("ok") is not True, "oracle returned an error")
    op = record.get("op")
    _reject(op not in fields, f"unknown response operation {op!r}")
    _exact_keys(record, COMMON | fields[op], "response")
    _reject(record["schema"] != expected["schema"], "response schema mismatch")
    for name in ("tool_identity", "physics_identity", "map_sha256", "map_checksum"):
        _reject(record[name] != expected[name], f"{name} mismatch")
    if op == "identity":
        _reject(record["provenance"] != expected["provenance"], "tool provenance mismatch")
        _reject(record["source"] != expected["source"], "source identity mismatch")
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("cm", "pmove"), required=True)
    parser.add_argument("--identity", type=Path, required=True)
    args = parser.parse_args()
    expected = json.loads(args.identity.read_text(encoding="utf-8"))
    try:
        for line in sys.stdin:
            validate_response(json.loads(line), expected, args.kind)
    except (IdentityMismatch, json.JSONDecodeError) as error:
        print(f"identity admission rejected: {error}", file=sys.stderr)
        return 65
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
