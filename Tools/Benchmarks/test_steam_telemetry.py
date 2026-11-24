"""Tests for the host-side benchmark health gate."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import steam_telemetry


def sample(temperature, fan=True, clock=2200000000):
    return {"gpuJunctionMilliC": temperature, "gpuClockHz": clock, "fanActive": fan}


class SteamTelemetryTests(unittest.TestCase):
    def test_threshold_is_inclusive_and_any_inactive_fan_sample_invalidates(self):
        healthy = [sample(72000), sample(99999)]
        self.assertEqual(steam_telemetry.validate(healthy, 100000), [])
        self.assertEqual(len(steam_telemetry.validate(healthy + [sample(100000)], 100000)), 1)
        self.assertEqual(len(steam_telemetry.validate(healthy + [sample(80000, fan=False)], 100000)), 1)

    def test_missing_required_sensors_invalidate(self):
        self.assertTrue(steam_telemetry.validate([sample(71000, clock=None)], 100000))
        self.assertTrue(steam_telemetry.validate([sample(71000)], None))

    def test_an_invalid_run_marks_every_scene_and_preserves_existing_failures(self):
        with tempfile.TemporaryDirectory() as temp:
            result = Path(temp) / "result.json"
            sidecar = Path(temp) / "result.steam-telemetry.json"
            result.write_text(json.dumps({"scenes": {"Forest": {"valid": True, "problems": []},
                                                    "City": {"valid": False, "problems": ["bad swapchain"]}}}))
            self.assertEqual(steam_telemetry.record_result(result, sidecar, [sample(100000)], 100000, 0), 1)
            stored = json.loads(result.read_text())
            self.assertFalse(stored["scenes"]["Forest"]["valid"])
            self.assertFalse(stored["scenes"]["City"]["valid"])
            self.assertEqual(stored["scenes"]["City"]["problems"][0], "bad swapchain")
            self.assertFalse(json.loads(sidecar.read_text())["valid"])

    def test_healthy_wrapper_preserves_scene_status(self):
        with tempfile.TemporaryDirectory() as temp:
            result = Path(temp) / "result.json"
            sidecar = Path(temp) / "result.steam-telemetry.json"
            result.write_text(json.dumps({"scenes": {"Forest": {"valid": True, "problems": []}}}))
            self.assertEqual(steam_telemetry.record_result(result, sidecar, [sample(75000)], 100000, 0), 0)
            self.assertTrue(json.loads(result.read_text())["scenes"]["Forest"]["valid"])


if __name__ == "__main__":
    unittest.main()
