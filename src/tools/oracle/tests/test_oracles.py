from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from bsp_fixture import write_bsp

TOOL_DIR = Path(__file__).resolve().parents[1]
ROOT = TOOL_DIR.parents[2]
CM = ROOT / "release" / "q2-cm-oracle"
PMOVE = ROOT / "release" / "q2-pmove-oracle"
sys.path.insert(0, str(TOOL_DIR))
from identity_validation import IdentityMismatch, validate_identity_record, validate_response

SOLID = 1
WINDOW = 2
LAVA = 8
SLIME = 16
WATER = 32
PLAYERCLIP = 0x10000
CURRENT_0 = 0x40000
LADDER = 0x20000000
PLAYER_MINS = [-16, -16, -24]
STAND_MAXS = [16, 16, 32]
CROUCH_MAXS = [16, 16, 4]


def invoke(binary: Path, bsp: Path, requests: list[dict]) -> list[dict]:
    payload = "".join(json.dumps(request, separators=(",", ":")) + "\n" for request in requests)
    result = subprocess.run(
        [str(binary), "--map", str(bsp)], input=payload, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"{binary.name} exited {result.returncode}: {result.stderr}")
    records = [json.loads(line) for line in result.stdout.splitlines()]
    if len(records) != len(requests):
        raise AssertionError(f"expected {len(requests)} records, got {result.stdout!r}")
    return records


class OracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory(prefix="q2-oracle-")
        cls.dir = Path(cls.temp.name)
        cls.floor = cls.dir / "floor.bsp"
        write_bsp(cls.floor, brushes=[((-2048, -2048, -128), (2048, 2048, 0), SOLID)])
        cls.ceiling = cls.dir / "ceiling.bsp"
        write_bsp(cls.ceiling, brushes=[((-128, -128, 20), (128, 128, 128), SOLID)])
        cls.wall = cls.dir / "wall.bsp"
        write_bsp(cls.wall, brushes=[((-4, -256, -128), (4, 256, 256), SOLID)])
        cls.inline = cls.dir / "inline.bsp"
        write_bsp(
            cls.inline,
            brushes=[((-8, -24, -16), (8, 24, 16), SOLID)],
            inline_model_brush=0,
        )
        cls.step18 = cls.dir / "step18.bsp"
        cls.step19 = cls.dir / "step19.bsp"
        for height, destination in ((18, cls.step18), (19, cls.step19)):
            write_bsp(destination, brushes=[
                ((-2048, -2048, -128), (2048, 2048, 0), SOLID),
                ((0, -256, 0), (2048, 256, height), SOLID),
            ])
        cls.cm_identity = invoke(CM, cls.floor, [{"id": "cm-id", "op": "identity"}])[0]
        cls.pmove_identity = invoke(PMOVE, cls.floor, [{"id": "pm-id", "op": "identity"}])[0]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    def test_all_required_point_contents(self) -> None:
        cases = {
            "solid": SOLID,
            "window": WINDOW,
            "playerclip": PLAYERCLIP,
            "water": WATER,
            "slime": SLIME,
            "lava": LAVA,
            "ladder": LADDER,
            "water_current": WATER | CURRENT_0,
        }
        for name, contents in cases.items():
            with self.subTest(name=name):
                bsp = self.dir / f"contents-{name}.bsp"
                write_bsp(bsp, split_contents=contents)
                responses = invoke(CM, bsp, [
                    {"id": "inside", "op": "point_contents", "point": [-1, 0, 0]},
                    {"id": "outside", "op": "point_contents", "point": [1, 0, 0]},
                ])
                self.assertEqual(responses[0]["contents"], contents)
                self.assertEqual(responses[1]["contents"], 0)

    def test_machine_readable_schemas(self) -> None:
        schemas = TOOL_DIR / "schemas"
        expected = {
            "q2-cm-oracle-v1.schema.json": "urn:q2-ml:q2-cm-oracle-v1",
            "q2-pmove-oracle-v1.schema.json": "urn:q2-ml:q2-pmove-oracle-v1",
            "q2-cm-oracle-v1.response.schema.json": "urn:q2-ml:q2-cm-oracle-v1:response",
            "q2-pmove-oracle-v1.response.schema.json": "urn:q2-ml:q2-pmove-oracle-v1:response",
            "q2-oracle-tool-identity-v1.schema.json": "urn:q2-ml:q2-oracle-tool-identity-v1",
            "q2-oracle-identity-v1.response.schema.json": "urn:q2-ml:q2-oracle-identity-v1:response",
        }
        for name, schema_id in expected.items():
            with self.subTest(schema=name):
                self.assertEqual(json.loads((schemas / name).read_text())["$id"], schema_id)
        cm_request = json.loads((schemas / "q2-cm-oracle-v1.schema.json").read_text())
        cm_response = json.loads((schemas / "q2-cm-oracle-v1.response.schema.json").read_text())
        for operation in ("transformed_point_contents", "transformed_box_trace"):
            self.assertIn(operation, cm_request["properties"]["op"]["enum"])
            self.assertIn(operation, cm_response["$defs"]["success"]["properties"]["op"]["enum"])
        self.assertEqual(cm_request["$defs"]["boundedCoordinate"]["maximum"], 1048576)
        self.assertEqual(cm_request["$defs"]["boundedAngle"]["maximum"], 360)

    def test_identity_admission_is_strict_and_fail_closed(self) -> None:
        validate_identity_record(self.cm_identity, "cm")
        validate_identity_record(self.pmove_identity, "pmove")
        point = invoke(CM, self.floor, [
            {"id": "p", "op": "point_contents", "point": [0, 0, 24]},
        ])[0]
        movement = invoke(PMOVE, self.floor, [{
            "id": "m", "op": "simulate", "origin": [0, 0, 24],
            "commands": [{"msec": 10}],
        }])[0]
        validate_response(point, self.cm_identity, "cm")
        validate_response(movement, self.pmove_identity, "pmove")

        mutations = (
            ("schema", lambda value: value.__setitem__("schema", "q2-cm-oracle-v0")),
            ("source", lambda value: value["source"].__setitem__("collision_sha256", "0" * 64)),
            ("build", lambda value: value["provenance"].__setitem__("build_identity_sha256", "1" * 64)),
            ("tool", lambda value: value.__setitem__("tool_identity", "2" * 64)),
            ("physics", lambda value: value.__setitem__("physics_identity", "3" * 64)),
            ("unknown", lambda value: value.__setitem__("uncontracted", True)),
        )
        for label, mutate in mutations:
            with self.subTest(mismatch=label):
                changed = copy.deepcopy(self.cm_identity)
                mutate(changed)
                with self.assertRaises(IdentityMismatch):
                    validate_response(changed, self.cm_identity, "cm")

    def test_source_closure_covers_all_build_inputs(self) -> None:
        spec = importlib.util.spec_from_file_location("oracle_gen_identity", TOOL_DIR / "gen_identity.py")
        self.assertIsNotNone(spec)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        closure = {path.relative_to(ROOT).as_posix() for path in module.source_closure(ROOT)}
        expected = set(module.COMPILED_SOURCES) | set(module.BUILD_INPUTS)
        expected |= {
            f"src/tools/oracle/schemas/{path.name}"
            for path in (TOOL_DIR / "schemas").glob("*.json")
        }
        self.assertTrue(expected <= closure)
        self.assertEqual(self.cm_identity["provenance"]["source_closure_count"], len(closure))

    def test_actual_build_identity_is_deterministic_and_flag_bound(self) -> None:
        with tempfile.TemporaryDirectory(prefix="q2-oracle-build-") as directory:
            build = Path(directory) / "build"
            binary_dir = Path(directory) / "bin"

            def make(extra: list[str]) -> dict:
                command = [
                    "make", "-C", str(TOOL_DIR), f"BUILD={build}", f"BIN={binary_dir}",
                    "all", *extra,
                ]
                result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, check=False)
                if result.returncode != 0:
                    self.fail(f"isolated oracle build failed:\n{result.stdout}")
                return invoke(binary_dir / "q2-cm-oracle", self.floor,
                              [{"id": "identity", "op": "identity"}])[0]

            baseline = make([])
            changed = make(["CFLAGS=-DQ2_ORACLE_IDENTITY_TEST=1"])
            repeated = make(["CFLAGS=-DQ2_ORACLE_IDENTITY_TEST=1"])
            self.assertEqual(changed, repeated)
            self.assertEqual(baseline["provenance"]["source_closure_sha256"],
                             changed["provenance"]["source_closure_sha256"])
            self.assertNotEqual(baseline["provenance"]["build_identity_sha256"],
                                changed["provenance"]["build_identity_sha256"])
            self.assertNotEqual(baseline["tool_identity"], changed["tool_identity"])

    def test_stationary_and_swept_standing_and_crouched_hulls(self) -> None:
        requests = [
            {"id": "stand", "op": "box_trace", "start": [0, 0, 0], "end": [0, 0, 0],
             "mins": PLAYER_MINS, "maxs": STAND_MAXS, "mask": SOLID},
            {"id": "crouch", "op": "box_trace", "start": [0, 0, 0], "end": [0, 0, 0],
             "mins": PLAYER_MINS, "maxs": CROUCH_MAXS, "mask": SOLID},
            {"id": "swept", "op": "box_trace", "start": [-64, 0, 24], "end": [64, 0, 24],
             "mins": PLAYER_MINS, "maxs": STAND_MAXS, "mask": SOLID},
        ]
        stand, crouch = invoke(CM, self.ceiling, requests[:2])
        swept = invoke(CM, self.wall, requests[2:])[0]
        self.assertTrue(stand["startsolid"])
        self.assertTrue(stand["allsolid"])
        self.assertFalse(crouch["startsolid"])
        self.assertEqual(crouch["fraction"], 1)
        self.assertGreater(swept["fraction"], 0)
        self.assertLess(swept["fraction"], 1)
        self.assertAlmostEqual(swept["endpos"][0], -20.03125, places=5)

    def test_transformed_inline_model_translation_and_rotation_goldens(self) -> None:
        responses = invoke(CM, self.inline, [
            {
                "id": "translated-inside", "op": "transformed_point_contents",
                "point": [100, 70, 0], "headnode": 0,
                "origin": [100, 50, 0], "angles": [0, 0, 0],
            },
            {
                "id": "translated-outside", "op": "transformed_point_contents",
                "point": [120, 50, 0], "headnode": 0,
                "origin": [100, 50, 0], "angles": [0, 0, 0],
            },
            {
                "id": "rotated-inside", "op": "transformed_point_contents",
                "point": [120, 50, 0], "headnode": 0,
                "origin": [100, 50, 0], "angles": [0, 90, 0],
            },
            {
                "id": "rotated-outside", "op": "transformed_point_contents",
                "point": [100, 70, 0], "headnode": 0,
                "origin": [100, 50, 0], "angles": [0, 90, 0],
            },
            {
                "id": "translated-trace", "op": "transformed_box_trace",
                "start": [50, 50, 0], "end": [150, 50, 0],
                "mins": [0, 0, 0], "maxs": [0, 0, 0], "mask": SOLID,
                "headnode": 0, "origin": [100, 50, 0], "angles": [0, 0, 0],
            },
            {
                "id": "rotated-trace", "op": "transformed_box_trace",
                "start": [50, 50, 0], "end": [150, 50, 0],
                "mins": [0, 0, 0], "maxs": [0, 0, 0], "mask": SOLID,
                "headnode": 0, "origin": [100, 50, 0], "angles": [0, 90, 0],
            },
        ])
        self.assertEqual([record["contents"] for record in responses[:4]], [SOLID, 0, SOLID, 0])
        self.assertAlmostEqual(responses[4]["fraction"], 0.4196875, places=7)
        self.assertAlmostEqual(responses[4]["endpos"][0], 91.96875, places=5)
        self.assertAlmostEqual(responses[5]["fraction"], 0.2596875, places=7)
        self.assertAlmostEqual(responses[5]["endpos"][0], 75.96875, places=5)
        self.assertEqual(responses[5]["origin"], [100, 50, 0])
        self.assertEqual(responses[5]["angles"], [0, 90, 0])
        inline_identity = invoke(CM, self.inline, [{"id": "identity", "op": "identity"}])[0]
        validate_response(responses[2], inline_identity, "cm")
        validate_response(responses[5], inline_identity, "cm")
        unsealed = copy.deepcopy(responses[5])
        del unsealed["angles"]
        with self.assertRaises(IdentityMismatch):
            validate_response(unsealed, inline_identity, "cm")

    def test_transformed_inline_model_requests_fail_closed(self) -> None:
        base = {
            "id": "bad", "op": "transformed_point_contents", "point": [100, 50, 0],
            "headnode": 0, "origin": [100, 50, 0], "angles": [0, 0, 0],
        }
        cases = []
        missing_origin = copy.deepcopy(base)
        del missing_origin["origin"]
        cases.append((missing_origin, "invalid_transform"))
        nonfinite_origin = copy.deepcopy(base)
        nonfinite_origin["origin"] = [float("nan"), 50, 0]
        cases.append((nonfinite_origin, "invalid_transform"))
        oversized_origin = copy.deepcopy(base)
        oversized_origin["origin"] = [1048577, 50, 0]
        cases.append((oversized_origin, "invalid_transform"))
        oversized_angle = copy.deepcopy(base)
        oversized_angle["angles"] = [0, 361, 0]
        cases.append((oversized_angle, "invalid_transform"))
        invalid_headnode = copy.deepcopy(base)
        invalid_headnode["headnode"] = 1
        cases.append((invalid_headnode, "inline_headnode"))
        for request, error in cases:
            with self.subTest(error=error, request=request):
                response = invoke(CM, self.inline, [request])[0]
                self.assertFalse(response["ok"])
                self.assertEqual(response["error"], error)

        world_only = invoke(CM, self.floor, [base])[0]
        self.assertFalse(world_only["ok"])
        self.assertEqual(world_only["error"], "inline_headnode")
        inverted = {
            "id": "inverted", "op": "transformed_box_trace",
            "start": [0, 0, 0], "end": [1, 0, 0],
            "mins": [1, 0, 0], "maxs": [-1, 0, 0], "mask": SOLID,
            "headnode": 0, "origin": [0, 0, 0], "angles": [0, 0, 0],
        }
        response = invoke(CM, self.inline, [inverted])[0]
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"], "invalid_trace")

    def test_pvs_is_coarse_and_trace_is_authoritative(self) -> None:
        pvs = invoke(CM, self.wall, [
            {"id": "pvs", "op": "pvs", "from": [-64, 0, 24], "to": [64, 0, 24]},
            {"id": "los", "op": "box_trace", "start": [-64, 0, 24], "end": [64, 0, 24],
             "mins": [0, 0, 0], "maxs": [0, 0, 0], "mask": SOLID},
        ])
        self.assertTrue(pvs[0]["potentially_visible"])
        self.assertLess(pvs[1]["fraction"], 1)

    def test_invalid_batch_is_one_atomic_error_record(self) -> None:
        response = invoke(PMOVE, self.floor, [{
            "id": "bad", "op": "simulate", "origin": [0, 0, 24],
            "commands": [{"msec": 100}, {"msec": 0}],
        }])[0]
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"], "invalid_command")

    def test_exact_step_size_boundary(self) -> None:
        request = {
            "id": "step", "op": "simulate", "origin": [-48, 0, 24],
            "pm_flags": 4, "snapinitial": False,
            "commands": [{"msec": 100, "angles": [0, 0, 0], "forwardmove": 300} for _ in range(8)],
        }
        at_18 = invoke(PMOVE, self.step18, [request])[0]
        at_19 = invoke(PMOVE, self.step19, [request])[0]
        self.assertGreater(at_18["final"]["origin"][0], 0)
        self.assertEqual(at_18["final"]["origin_fixed"][2], 337)
        self.assertLess(at_19["final"]["origin"][0], 0)
        self.assertEqual(at_19["final"]["origin_fixed"][2], 193)

    def test_jump_drop_landing_and_golden_determinism(self) -> None:
        request = {
            "id": "trajectory", "op": "simulate", "origin": [0, 0, 24],
            "pm_flags": 4, "snapinitial": False,
            "commands": ([{"msec": 50, "upmove": 200}] + [{"msec": 50} for _ in range(30)]),
        }
        first = invoke(PMOVE, self.floor, [request])[0]
        second = invoke(PMOVE, self.floor, [request])[0]
        self.assertEqual(first, second)
        self.assertEqual(first["command_count"], 31)
        self.assertTrue(first["final"]["grounded"])
        # Quake II deliberately leaves the resting hull one 1/8-unit fixed
        # coordinate above the mathematical floor through DIST_EPSILON.
        self.assertEqual(first["final"]["origin_fixed"], [0, 0, 193])
        self.assertEqual(first["final"]["velocity_fixed"], [0, 0, 0])
        identity = invoke(PMOVE, self.floor, [{"id": "identity", "op": "identity"}])[0]
        self.assertEqual(len(identity["physics_identity"]), 64)
        self.assertEqual(len(identity["map_sha256"]), 64)


if __name__ == "__main__":
    unittest.main()
