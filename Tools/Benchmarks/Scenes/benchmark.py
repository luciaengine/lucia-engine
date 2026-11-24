#!/usr/bin/env python3
"""Lucia's fixed performance benchmark: runs the Player on the benchmark scenes and compares results.

  benchmark.py run --player <Player executable> --out <result.json> [--scenes Forest,City] [--work <dir>]
  benchmark.py noise <result.json> <result.json> ... --out <noise.json>
  benchmark.py compare <base.json> <head.json> [--noise <noise.json>]

`run` prepares a work copy of Tests/Projects/PerformanceBenchmark (cooked cluster meshes, the crowd
model, render pipeline variants), then starts the Player once per scene with --benchmark, vsync off,
at the manifest's resolution. The Player holds the camera at each pose, warms up, records the frame
times with GPU profiling off and the per-pass GPU times with it on. The result holds the build SHA,
the GPU, the driver and every pose. docs/development/performance-benchmark.mdx is the guide.
Standard library only: the target machine has Python and no other tools.
"""

import argparse
import base64
import datetime
import hashlib
import json
import math
import os
import platform
import random
import re
import shutil
import statistics
import struct
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
PROJECT = REPO_ROOT / "Tests" / "Projects" / "PerformanceBenchmark"
FOX_SOURCE = REPO_ROOT / "Tests" / "EditorHarness" / "rg2-gate1-content" / "Assets" / "Models" / "Fox"
# The Player loads the default UI font from the project's Assets/Fonts.
FONT_SOURCE = REPO_ROOT / "Engine" / "Modules" / "UI" / "Examples" / "Resources" / "Fonts" / "Roboto-Regular.ttf"
# A Player log line that holds one of these tells that scene content did not load or a feature is off.
FEATURE_FAILURE_LINES = ("no registered type is named", "Asset file does not exist", "Cluster LOD fine pages")
RESULT_FORMAT = 1
# A comparison flags a metric when it is slower by more than the noise threshold. Without a noise
# file, or below it, this floor applies.
DEFAULT_THRESHOLD_PCT = 3.0
# A pass that takes less than this, in both runs, is never flagged. Passes this small have bimodal times
# between runs of one build (0.03 to 0.28 ms on the Steam Machine), so their relative change is noise.
# Their time still counts in the GPU total.
PASS_FLOOR_MS = 0.3
# The per-tile passes of one view render different tiles in each run, so they compare as one summed row.
TILE_PASS = re.compile(r"^(.*\.Tile)\d+$")
# The noise threshold of a metric is this many relative standard deviations of the repeat runs.
NOISE_SIGMAS = 3.0
# A scene is not valid when the 1-minute load average is above this when it ends. The Player's own
# threads raise the load, so this limit is above --max-load; other work on the machine passes it.
MAX_LOAD_AFTER_SCENE = 6.0


# ---------------------------------------------------------------- statistics

def percentile(values, fraction):
    """The nearest-rank percentile: the smallest value with at least `fraction` of values at or below it."""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(fraction * len(ordered)))
    return ordered[rank - 1]


def summarize(values):
    if not values:
        return None
    return {"median": statistics.median(values), "p99": percentile(values, 0.99),
            "mean": statistics.fmean(values), "min": min(values), "max": max(values), "count": len(values)}


def reduce_pose(raw_pose, required_passes):
    """The statistics of one pose from the Player's raw frames."""
    profiled = raw_pose.get("profiledFrames", [])
    per_pass = {}
    for frame in profiled:
        for name, gpu_ms, counted in frame["passes"]:
            if counted:
                per_pass.setdefault(name, []).append(gpu_ms)
    passes = sorted(({"name": name, "medianMs": statistics.median(values), "frames": len(values)}
                     for name, values in per_pass.items()), key=lambda p: -p["medianMs"])
    missing = [token for token in required_passes if not any(token in name for name in per_pass)]
    return {"name": raw_pose["name"], "position": raw_pose["position"], "yawDeg": raw_pose["yawDeg"],
            "pitchDeg": raw_pose["pitchDeg"], "frameMs": summarize(raw_pose["frameMs"]),
            "cpuMs": summarize(raw_pose["cpuMs"]), "waitMs": summarize(raw_pose["waitMs"]),
            "gpuMs": summarize([f["distinctGpuMs"] for f in profiled]), "passes": passes,
            "missingPasses": missing, "monitorPeaks": raw_pose.get("monitorPeaks", {})}


# ---------------------------------------------------------------- content

def building_gltf(index):
    """One unique procedural building as a glTF document: a tower of stacked, inset floor boxes."""
    rng = random.Random(9000 + index)
    width, depth = rng.uniform(10, 26), rng.uniform(10, 26)
    floors = rng.randint(10, 50)
    floor_height = rng.uniform(3.0, 4.2)
    columns = rng.randint(12, 32)
    positions, normals, indices = [], [], []

    def quad(corners, normal):
        base = len(positions)
        positions.extend(corners)
        normals.extend([normal] * 4)
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def box(x0, y0, z0, x1, y1, z1):
        quad([(x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0)], (0, 0, -1))
        quad([(x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (x0, y0, z1)], (0, 0, 1))
        quad([(x0, y0, z1), (x0, y1, z1), (x0, y1, z0), (x0, y0, z0)], (-1, 0, 0))
        quad([(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)], (1, 0, 0))
        quad([(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)], (0, 1, 0))

    for floor in range(floors):
        y0 = floor * floor_height
        inset = 0.25 * math.sin(floor * 0.7 + index)
        box(-width / 2 - inset, y0, -depth / 2 - inset, width / 2 + inset, y0 + 0.4, depth / 2 + inset)
        # Window mullions: thin columns around the floor, so each building has its own silhouette and detail.
        for c in range(columns):
            t = (c + 0.5) / columns
            for x, z in ((-width / 2 + t * width, -depth / 2), (-width / 2 + t * width, depth / 2),
                         (-width / 2, -depth / 2 + t * depth), (width / 2, -depth / 2 + t * depth)):
                box(x - 0.15, y0 + 0.4, z - 0.15, x + 0.15, y0 + floor_height, z + 0.15)
        box(-width / 2 + 0.4, y0 + 0.4, -depth / 2 + 0.4, width / 2 - 0.4, y0 + floor_height, depth / 2 - 0.4)

    position_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
    normal_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
    index_bytes = b"".join(struct.pack("<I", i) for i in indices)
    data = position_bytes + normal_bytes + index_bytes
    lo = [min(p[axis] for p in positions) for axis in range(3)]
    hi = [max(p[axis] for p in positions) for axis in range(3)]
    return {
        "asset": {"version": "2.0", "generator": "Lucia benchmark.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": f"Building{index:02d}"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}]}],
        "buffers": [{"byteLength": len(data),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode("ascii")}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(position_bytes), "target": 34962},
                        {"buffer": 0, "byteOffset": len(position_bytes), "byteLength": len(normal_bytes), "target": 34962},
                        {"buffer": 0, "byteOffset": len(position_bytes) + len(normal_bytes),
                         "byteLength": len(index_bytes), "target": 34963}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": lo, "max": hi},
                      {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5125, "count": len(indices), "type": "SCALAR"}],
    }


def find_tool(player, name):
    """A tool built beside the Player: <bin>/<config>/Tools/<name>."""
    for parent in player.parents:
        candidate = parent / "Tools" / name
        for path in (candidate, candidate.with_suffix(".exe")):
            if path.is_file():
                return path
    return None


def find_default_pipeline(player):
    for parent in list(player.parents)[:4]:
        found = sorted(parent.rglob("RenderPipelines/ForwardPlus.rendergraph"))
        if found:
            return found[0]
    return None


def prepare(work, manifest, scenes, player):
    """A work copy of the project with everything the selected scenes need."""
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    shutil.copytree(PROJECT, project)
    (project / "Assets" / "Fonts").mkdir(parents=True, exist_ok=True)
    shutil.copy2(FONT_SOURCE, project / "Assets" / "Fonts" / FONT_SOURCE.name)
    needs = {item for name in scenes for item in manifest["scenes"][name].get("content", [])}
    if "fox" in needs:
        shutil.copytree(FOX_SOURCE, project / "Assets" / "Models" / "Fox")
    if "city" in needs:
        cook = find_tool(player, "MeshClusterCook")
        if not cook:
            sys.exit("benchmark: the City scene needs MeshClusterCook beside the Player. Build the "
                     "MeshClusterCook target (configure with -DBUILD_TOOLS=ON).")
        # The cache is per cook binary: a new cook can write a format the old files are not.
        cache = work / f"city-cache-{hashlib.sha256(cook.read_bytes()).hexdigest()[:16]}"
        cache.mkdir(parents=True, exist_ok=True)
        target = project / "Assets" / "Models" / "City"
        target.mkdir(parents=True, exist_ok=True)
        for index in range(manifest["city"]["uniqueBuildings"]):
            name = f"Building{index:02d}"
            cooked = cache / f"{name}.gecluster"
            if not cooked.exists():
                source = cache / f"{name}.gltf"
                source.write_text(json.dumps(building_gltf(index)), encoding="utf-8")
                result = subprocess.run([str(cook), "--input", str(source), "--output", str(cooked)],
                                        capture_output=True, text=True)
                if result.returncode != 0 or not cooked.exists():
                    sys.exit(f"benchmark: MeshClusterCook failed on {source} with exit {result.returncode}:\n"
                             f"{result.stdout[-2000:]}{result.stderr[-2000:]}")
            shutil.copy2(cooked, target / cooked.name)
            sidecar = cache / ".cluster"
            if sidecar.is_dir():
                shutil.copytree(sidecar, target / ".cluster", dirs_exist_ok=True)
    for name in scenes:
        options = manifest["scenes"][name].get("pipeline")
        if not options:
            continue
        default = find_default_pipeline(player)
        if not default:
            sys.exit("benchmark: no staged RenderPipelines/ForwardPlus.rendergraph beside the Player. "
                     "Build the Player target so that its assets are staged.")
        graph = json.loads(default.read_text(encoding="utf-8"))
        shadow_nodes = [n for n in graph.get("passes", []) if n.get("type") == "ShadowMap"]
        if not shadow_nodes:
            sys.exit(f"benchmark: {default} has no ShadowMap node to configure for {name}.")
        for node in shadow_nodes:
            node.update(options)
        out = project / "Assets" / "RenderPipelines" / f"Benchmark{name}.rendergraph"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(graph, indent=2), encoding="utf-8")
    return project


# ---------------------------------------------------------------- machine

def build_sha(explicit):
    if explicit:
        return explicit
    try:
        return subprocess.run(["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        sys.exit("benchmark: no git checkout to read the build SHA from. Pass --build-sha <sha>.")


def gpu_info():
    """The first hardware Vulkan device from vulkaninfo, or nulls when vulkaninfo is absent."""
    try:
        text = subprocess.run(["vulkaninfo", "--summary"], capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        return {"name": None, "driver": None, "driverInfo": None, "apiVersion": None}
    devices, current = [], {}
    for line in text.splitlines():
        key, _, value = line.strip().partition("=")
        key, value = key.strip(), value.strip()
        if key == "apiVersion" and "name" in current:
            devices.append(current)
            current = {}
        if key in ("apiVersion", "deviceName", "driverName", "driverInfo"):
            current[{"deviceName": "name", "driverName": "driver"}.get(key, key)] = value
    if "name" in current:
        devices.append(current)
    hardware = [d for d in devices if d.get("driver") not in ("llvmpipe", None)]
    return (hardware or devices or [{"name": None, "driver": None, "driverInfo": None, "apiVersion": None}])[0]


def cpu_name():
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or None


def load_average():
    try:
        return round(os.getloadavg()[0], 2)
    except (AttributeError, OSError):
        return None


# ---------------------------------------------------------------- run

def feature_failures(log_text):
    """The first log line of each kind in FEATURE_FAILURE_LINES."""
    found = {}
    for line in log_text.splitlines():
        for token in FEATURE_FAILURE_LINES:
            if token in line and token not in found:
                found[token] = line.split("] ", 3)[-1].strip()
    return list(found.values())


def parse_swapchain(log_text):
    """(width, height, presentMode) from the device's swapchain line, or None."""
    for line in log_text.splitlines():
        if "Created window swapchain" in line:
            size = line.split("Created window swapchain", 1)[1].split()[0].rstrip(",")
            width, _, height = size.partition("x")
            mode = None
            for other in log_text.splitlines():
                if "presentMode=" in other:
                    mode = other.split("presentMode=", 1)[1].split()[0]
            return int(width), int(height), mode
    return None


def run_scene(name, settings, manifest, project, player, out_dir, timeout, check_load):
    width, height = manifest["resolution"]
    config = {"gameName": f"LuciaBenchmark{name}", "startupScene": settings["scene"],
              "window": {"width": width, "height": height, "vsync": False}}
    if settings.get("pipeline"):
        config["renderPipeline"] = f"RenderPipelines/Benchmark{name}.rendergraph"
    plan = {**manifest["run"], "poses": settings["poses"]}
    required_monitors = settings.get("requiredActiveMonitors", [])
    if required_monitors:
        plan["activeMonitors"] = required_monitors
    config_path, plan_path = out_dir / f"{name}.game.config", out_dir / f"{name}.plan.json"
    raw_path, log_path = out_dir / f"{name}.raw.json", out_dir / f"{name}.log"
    config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
    plan_path.write_text(json.dumps(plan, indent=2), encoding="utf-8")
    raw_path.unlink(missing_ok=True)
    env = {**os.environ, **settings.get("environment", {})}
    command = [str(player), "--config", str(config_path), "--asset-root", str(project / "Assets"),
               "--silent-audio", "--benchmark", str(plan_path), "--benchmark-output", str(raw_path)]
    print(f"[{name}] {' '.join(command)}", flush=True)
    load_before = load_average()
    with open(log_path, "w", encoding="utf-8") as log:
        try:
            exit_code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env, cwd=player.parent,
                                       timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            exit_code = "timeout"
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    problems = []
    if exit_code != 0:
        problems.append(f"the Player exited with {exit_code}; see {log_path}")
    swapchain = parse_swapchain(log_text)
    if not swapchain:
        problems.append("the log has no swapchain line, so the resolution and present mode are unknown")
    else:
        if (swapchain[0], swapchain[1]) != (width, height):
            problems.append(f"the swapchain is {swapchain[0]}x{swapchain[1]}, not {width}x{height}")
        if swapchain[2] == "FIFO":
            problems.append("the swapchain presents with FIFO (vsync), so the frame times are display-paced")
    problems += [f"the log says: {line}" for line in feature_failures(log_text)]
    result = {"scene": settings["scene"], "description": settings.get("description"),
              "environment": settings.get("environment", {}), "pipeline": settings.get("pipeline"),
              "swapchain": swapchain and {"width": swapchain[0], "height": swapchain[1], "presentMode": swapchain[2]},
              "loadBefore": load_before, "loadAfter": load_average(), "poses": []}
    if raw_path.exists():
        raw = json.loads(raw_path.read_text(encoding="utf-8"))
        for raw_pose in raw["poses"]:
            pose = reduce_pose(raw_pose, settings.get("requiredPasses", []))
            if pose["missingPasses"]:
                problems.append(f"pose '{pose['name']}' ran without the passes {pose['missingPasses']}")
            if not pose["gpuMs"]:
                problems.append(f"pose '{pose['name']}' has no resolved GPU timings")
            result["poses"].append(pose)
        missing = [name for name in required_monitors
                   if not any(pose["monitorPeaks"].get(name, 0) > 0 for pose in result["poses"])]
        if missing:
            problems.append(f"no profiled pose reported activity for {missing}")
    else:
        problems.append("the Player wrote no benchmark result")
    if check_load and result["loadAfter"] is not None and result["loadAfter"] > MAX_LOAD_AFTER_SCENE:
        problems.append(f"the 1-minute load average rose to {result['loadAfter']} during the scene, above "
                        f"{MAX_LOAD_AFTER_SCENE}: other work shared the machine")
    result["problems"] = problems
    result["valid"] = not problems
    return result


def command_run(args):
    manifest = json.loads((PROJECT / "benchmark.json").read_text(encoding="utf-8"))
    player = Path(args.player).resolve()
    if not player.is_file():
        sys.exit(f"benchmark: no Player at {player}. Build the Player target in Release first.")
    scenes = args.scenes.split(",") if args.scenes else list(manifest["scenes"])
    unknown = [s for s in scenes if s not in manifest["scenes"]]
    if unknown:
        sys.exit(f"benchmark: unknown scenes {unknown}; the manifest has {list(manifest['scenes'])}")
    load = load_average()
    if load is not None and args.max_load and load > args.max_load:
        sys.exit(f"benchmark: the 1-minute load average is {load}, above --max-load {args.max_load}. "
                 "Stop the other work on this machine, or raise --max-load and say so with the result.")
    out_path = Path(args.out).resolve()
    work = Path(args.work).resolve() if args.work else out_path.parent / (out_path.stem + "-work")
    work.mkdir(parents=True, exist_ok=True)
    project = prepare(work, manifest, scenes, player)

    result = {"format": RESULT_FORMAT, "label": args.label, "buildSha": build_sha(args.build_sha),
              "configuration": args.configuration,
              "date": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
              "machine": {"gpu": gpu_info(), "cpu": cpu_name(), "cores": os.cpu_count(), "os": platform.platform()},
              "resolution": manifest["resolution"], "run": manifest["run"], "scenes": {}}
    for name in scenes:
        result["scenes"][name] = run_scene(name, manifest["scenes"][name], manifest, project, player, work,
                                           args.timeout, bool(args.max_load))
        scene = result["scenes"][name]
        for pose in scene["poses"]:
            frame, gpu = pose["frameMs"], pose["gpuMs"]
            print(f"[{name}] {pose['name']:<10} frame median {frame['median']:7.3f} ms p99 {frame['p99']:7.3f} ms "
                  f"cpu {pose['cpuMs']['median']:7.3f} ms gpu {gpu['median'] if gpu else float('nan'):7.3f} ms",
                  flush=True)
        for problem in scene["problems"]:
            print(f"[{name}] NOT VALID: {problem}", flush=True)
    out_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out_path}")
    return 0 if all(s["valid"] for s in result["scenes"].values()) else 1


# ---------------------------------------------------------------- noise and compare

def metrics(result):
    """{(scene, pose, metric): value} of the valid scenes of one result."""
    values = {}
    for scene_name, scene in result["scenes"].items():
        if not scene.get("valid"):
            continue
        for pose in scene["poses"]:
            key = (scene_name, pose["name"])
            values[key + ("frameMs.median",)] = pose["frameMs"]["median"]
            values[key + ("frameMs.p99",)] = pose["frameMs"]["p99"]
            values[key + ("cpuMs.median",)] = pose["cpuMs"]["median"]
            if pose["gpuMs"]:
                values[key + ("gpuMs.median",)] = pose["gpuMs"]["median"]
            for p in pose["passes"]:
                tile = TILE_PASS.match(p["name"])
                metric = key + ("pass:" + (tile.group(1) + "*" if tile else p["name"]),)
                values[metric] = values.get(metric, 0.0) + p["medianMs"]
    return values


def read_result(path):
    result = json.loads(Path(path).read_text(encoding="utf-8"))
    telemetry = result.get("steamTelemetry")
    if telemetry and not telemetry["valid"]:
        sys.exit(f"benchmark: {path} has invalid Steam telemetry: {', '.join(telemetry['problems'])}")
    return result


def command_noise(args):
    results = [read_result(p) for p in args.results]
    if len(results) < 2:
        sys.exit("benchmark: noise needs at least 2 results of one build; 5 separate runs are the norm.")
    shas = {r["buildSha"] for r in results}
    if len(shas) != 1:
        sys.exit(f"benchmark: noise needs results of one build; these have {sorted(shas)}.")
    per_run = [metrics(r) for r in results]
    keys = set.intersection(*(set(m) for m in per_run))
    thresholds = {}
    for key in sorted(keys):
        samples = [m[key] for m in per_run]
        mean = statistics.fmean(samples)
        if mean <= 0:
            continue
        relative = statistics.stdev(samples) / mean * 100.0
        thresholds["|".join(key)] = {"thresholdPct": max(DEFAULT_THRESHOLD_PCT, NOISE_SIGMAS * relative),
                                     "relativeStdevPct": relative, "mean": mean, "runs": len(samples)}
    noise = {"buildSha": shas.pop(), "runs": len(results), "sigmas": NOISE_SIGMAS,
             "floorPct": DEFAULT_THRESHOLD_PCT, "metrics": thresholds}
    Path(args.out).write_text(json.dumps(noise, indent=2) + "\n", encoding="utf-8")
    worst = sorted(thresholds.items(), key=lambda kv: -kv[1]["relativeStdevPct"])[:5]
    for key, value in worst:
        print(f"noisiest {key}: {value['relativeStdevPct']:.2f}% stdev over {value['runs']} runs")
    print(f"wrote {args.out} ({len(thresholds)} metrics)")
    return 0


def compare(base, head, noise):
    """Return compared rows, regressions, and baseline poses without a valid comparison."""
    base_values, head_values = metrics(base), metrics(head)
    thresholds = (noise or {}).get("metrics", {})
    rows, regressions = [], []
    for key in sorted(set(base_values) & set(head_values)):
        before, after = base_values[key], head_values[key]
        if not math.isfinite(before) or not math.isfinite(after) or before <= 0:
            continue
        if key[2] == "frameMs.median" and after <= 0:
            continue
        delta_pct = (after - before) / before * 100.0
        threshold = thresholds.get("|".join(key), {}).get("thresholdPct", DEFAULT_THRESHOLD_PCT)
        is_pass = key[2].startswith("pass:")
        regressed = delta_pct > threshold and not (is_pass and max(before, after) < PASS_FLOOR_MS)
        row = {"scene": key[0], "pose": key[1], "metric": key[2], "base": before, "head": after,
               "deltaPct": delta_pct, "thresholdPct": threshold, "regressed": regressed}
        rows.append(row)
        if regressed:
            regressions.append(row)
    baseline_poses = {key[:2] for key in base_values if key[2] == "frameMs.median"}
    compared_poses = {(row["scene"], row["pose"]) for row in rows if row["metric"] == "frameMs.median"}
    missing = sorted(baseline_poses - compared_poses)
    return rows, regressions, missing


def command_compare(args):
    base = read_result(args.base)
    head = read_result(args.head)
    noise = json.loads(Path(args.noise).read_text(encoding="utf-8")) if args.noise else None
    if base["machine"]["gpu"] != head["machine"]["gpu"] or base["resolution"] != head["resolution"]:
        print("WARNING: the two results come from different GPUs, drivers or resolutions; they do not compare.")
    rows, regressions, missing = compare(base, head, noise)
    print(f"base {base['buildSha'][:12]}  head {head['buildSha'][:12]}  "
          f"noise {'from ' + args.noise if noise else f'floor {DEFAULT_THRESHOLD_PCT}%'}")
    for row in rows:
        if row["metric"].startswith("pass:") and not row["regressed"] and not args.all_passes:
            continue
        flag = "REGRESSION" if row["regressed"] else ""
        print(f"{row['scene']:<10} {row['pose']:<10} {row['metric']:<44} {row['base']:9.3f} -> {row['head']:9.3f} ms "
              f"{row['deltaPct']:+7.2f}% (limit {row['thresholdPct']:.1f}%) {flag}")
    for scene, pose in missing:
        print(f"{scene} / {pose}: no valid frame time comparison for this baseline pose")
    has_frame_comparison = any(row["metric"] == "frameMs.median" for row in rows)
    if not has_frame_comparison:
        print("benchmark: no valid frame time comparisons")
    print(f"{len(regressions)} regression(s)")
    return 1 if regressions or missing or not has_frame_comparison else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="run the Player on the benchmark scenes")
    run.add_argument("--player", required=True, help="the Release Player executable")
    run.add_argument("--out", required=True, help="the result JSON to write")
    run.add_argument("--scenes", help="comma-separated scene names; all by default")
    run.add_argument("--work", help="the work folder; default <out stem>-work beside --out")
    run.add_argument("--label", default="", help="a free label stored in the result")
    run.add_argument("--build-sha", help="the SHA of the build when the source is not a git checkout")
    run.add_argument("--configuration", default="Release", help="the build configuration, stored in the result")
    run.add_argument("--max-load", type=float, default=1.5, help="refuse to start above this load average; 0 turns the check off")
    run.add_argument("--timeout", type=float, default=1800, help="seconds per scene before the Player is stopped")
    noise = sub.add_parser("noise", help="measure the noise threshold from repeat results of one build")
    noise.add_argument("results", nargs="+")
    noise.add_argument("--out", required=True)
    cmp = sub.add_parser("compare", help="compare two results and flag regressions")
    cmp.add_argument("base")
    cmp.add_argument("head")
    cmp.add_argument("--noise", help="a noise file from `noise`; without it the floor applies")
    cmp.add_argument("--all-passes", action="store_true", help="print every pass, not only regressed passes")
    args = parser.parse_args()
    return {"run": command_run, "noise": command_noise, "compare": command_compare}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
