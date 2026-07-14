from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from bsp_fixture import write_bsp

TOOL_DIR = Path(__file__).resolve().parents[1]
ROOT = TOOL_DIR.parents[2]
CM = ROOT / "release" / "q2-cm-oracle"
PMOVE = ROOT / "release" / "q2-pmove-oracle"

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
        cls.step18 = cls.dir / "step18.bsp"
        cls.step19 = cls.dir / "step19.bsp"
        for height, destination in ((18, cls.step18), (19, cls.step19)):
            write_bsp(destination, brushes=[
                ((-2048, -2048, -128), (2048, 2048, 0), SOLID),
                ((0, -256, 0), (2048, 256, height), SOLID),
            ])

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
        cm = json.loads((schemas / "q2-cm-oracle-v1.schema.json").read_text())
        pmove = json.loads((schemas / "q2-pmove-oracle-v1.schema.json").read_text())
        self.assertEqual(cm["$id"], "urn:q2-ml:q2-cm-oracle-v1")
        self.assertEqual(pmove["$id"], "urn:q2-ml:q2-pmove-oracle-v1")

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
