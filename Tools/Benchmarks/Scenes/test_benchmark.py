#!/usr/bin/env python3
"""Tests of benchmark.py's reduction and comparison: python3 -m unittest Tools/Benchmarks/Scenes/test_benchmark.py"""

import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchmark  # noqa: E402


def raw_pose(frames, gpu_frames):
    return {"name": "a", "position": [0, 0, 0], "yawDeg": 0, "pitchDeg": 0, "frameMs": frames, "cpuMs": frames,
            "waitMs": [0.0] * len(frames),
            "profiledFrames": [{"distinctGpuMs": sum(p[1] for p in passes if p[2]), "passes": passes}
                               for passes in gpu_frames]}


def result(sha, frame_ms, pass_ms=1.0, valid=True):
    pose = benchmark.reduce_pose(raw_pose([frame_ms] * 10, [[["Shadow.Cascade0", pass_ms, True]]] * 4), [])
    return {"buildSha": sha, "machine": {"gpu": {"name": "gpu"}}, "resolution": [1920, 1080],
            "scenes": {"Forest": {"valid": valid, "poses": [pose]}}}


class ReduceTests(unittest.TestCase):
    def test_percentile_is_nearest_rank(self):
        values = list(range(1, 101))
        self.assertEqual(benchmark.percentile(values, 0.99), 99)
        self.assertEqual(benchmark.percentile(values, 0.5), 50)
        self.assertEqual(benchmark.percentile([7.0], 0.99), 7.0)

    def test_pose_counts_each_gpu_measurement_once_and_names_missing_passes(self):
        frames = [[["DDGI.Trace", 2.0, True], ["Shared", 5.0, False], ["Shadow", 1.0, True]]] * 3
        pose = benchmark.reduce_pose(raw_pose([10.0, 12.0, 30.0], frames), ["DDGI", "Ocean"])
        self.assertEqual(pose["frameMs"]["median"], 12.0)
        self.assertEqual(pose["frameMs"]["p99"], 30.0)
        self.assertEqual(pose["gpuMs"]["median"], 3.0)
        self.assertEqual([p["name"] for p in pose["passes"]], ["DDGI.Trace", "Shadow"])
        self.assertEqual(pose["missingPasses"], ["Ocean"])


class CompareTests(unittest.TestCase):
    def test_a_slowdown_beyond_the_threshold_is_a_regression_and_within_it_is_not(self):
        _, regressions, _ = benchmark.compare(result("a", 10.0), result("b", 10.2), None)
        self.assertEqual(regressions, [])
        _, regressions, _ = benchmark.compare(result("a", 10.0), result("b", 11.0), None)
        self.assertIn("frameMs.median", [r["metric"] for r in regressions])

    def test_a_tiny_pass_is_never_flagged(self):
        _, regressions, _ = benchmark.compare(result("a", 10.0, pass_ms=0.01), result("b", 10.0, pass_ms=0.03), None)
        self.assertEqual([r for r in regressions if r["metric"].startswith("pass:")], [])

    def test_a_scene_that_became_invalid_is_reported(self):
        _, _, missing = benchmark.compare(result("a", 10.0), result("b", 10.0, valid=False), None)
        self.assertEqual(missing, ["Forest"])

    def test_noise_raises_the_threshold_of_a_noisy_metric(self):
        with tempfile.TemporaryDirectory() as folder:
            paths = []
            for i, frame_ms in enumerate([10.0, 10.8, 9.2, 10.4, 9.6]):
                path = Path(folder) / f"r{i}.json"
                path.write_text(json.dumps(result("a", frame_ms)))
                paths.append(str(path))
            out = Path(folder) / "noise.json"
            benchmark.command_noise(SimpleNamespace(results=paths, out=str(out)))
            noise = json.loads(out.read_text())
        threshold = noise["metrics"]["Forest|a|frameMs.median"]["thresholdPct"]
        self.assertGreater(threshold, benchmark.DEFAULT_THRESHOLD_PCT)
        _, regressions, _ = benchmark.compare(result("a", 10.0), result("b", 10.5), noise)
        self.assertNotIn("frameMs.median", [r["metric"] for r in regressions])


class ContentTests(unittest.TestCase):
    def test_every_building_is_unique_and_valid_gltf(self):
        first, second = benchmark.building_gltf(0), benchmark.building_gltf(1)
        self.assertNotEqual(first["accessors"][0]["count"], second["accessors"][0]["count"])
        self.assertEqual(first["accessors"][2]["count"] % 3, 0)


if __name__ == "__main__":
    unittest.main()
