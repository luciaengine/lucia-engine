#!/usr/bin/env python3
"""Measure Steam Machine GPU conditions around a benchmark command on the host."""

import argparse
import datetime as dt
import json
from pathlib import Path
import subprocess
import sys
import tempfile

SAMPLE_INTERVAL = 2
FAN_SERVICE = "jupiter-fan-control.service"
GPU_SENSORS = {"gpuClockHz": "freq1_input", "gpuEdgeMilliC": "temp1_input",
               "gpuJunctionMilliC": "temp2_input", "gpuPowerMicrowatts": "power1_average",
               "gpuPowerCapMicrowatts": "power1_cap"}


def read_int(path):
    try:
        return int(path.read_text())
    except (OSError, ValueError):
        return None


def named_sensors(paths, driver):
    return [path for path in paths if (path / "name").is_file()
            and (path / "name").read_text().strip() == driver]


def fan_active():
    try:
        return subprocess.run(["systemctl", "is-active", "--quiet", FAN_SERVICE],
                              timeout=1, check=False).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def capture(gpu, cpu_sensor):
    values = {key: read_int(gpu / filename) for key, filename in GPU_SENSORS.items()}
    values["cpuTctlMilliC"] = read_int(cpu_sensor) if cpu_sensor else None
    values["time"] = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    values["fanActive"] = fan_active()
    return values


def validate(samples, limit):
    problems = []
    if limit is None or not samples or any(s["gpuJunctionMilliC"] is None or s["gpuClockHz"] is None for s in samples):
        problems.append("GPU clock, junction temperature, or throttle limit was unavailable")
    if limit is not None and any(s["gpuJunctionMilliC"] is not None and
                                 s["gpuJunctionMilliC"] >= limit for s in samples):
        problems.append(f"GPU junction reached its {limit / 1000:g} °C throttle limit")
    if any(not s["fanActive"] for s in samples):
        problems.append(f"{FAN_SERVICE} was inactive or unavailable")
    return problems


def write_json(path, data):
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as out:
        temporary = Path(out.name)
        try:
            json.dump(data, out, indent=2)
            out.write("\n")
            out.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def record_result(result_path, sample_path, samples, limit, exit_code):
    problems = validate(samples, limit)
    if exit_code:
        problems.append(f"Benchmark command exited {exit_code}")
    result = None
    if result_path.is_file():
        try:
            result = json.loads(result_path.read_text())
            if not isinstance(result.get("scenes"), dict) or not result["scenes"]:
                raise ValueError("no scene results")
        except (AttributeError, OSError, ValueError):
            result = None
    if result is None:
        problems.append("Benchmark command wrote no usable result")
    write_json(sample_path, {"valid": not problems, "problems": problems, "commandExit": exit_code,
                             "gpuThrottleMilliC": limit, "samples": samples})
    if result is not None:
        result["steamTelemetry"] = {"file": sample_path.name, "valid": not problems, "problems": problems}
        if problems:
            for scene in result["scenes"].values():
                scene["valid"] = False
                scene.setdefault("problems", []).extend(problems)
        write_json(result_path, result)
    for problem in problems:
        print(f"NOT VALID: {problem}", file=sys.stderr)
    return int(bool(problems))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", required=True, type=Path, help="new benchmark JSON path, as seen on the host")
    parser.add_argument("--gpu-hwmon", type=Path, help="AMDGPU hwmon directory if the host has more than one")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="benchmark command after --")
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("pass a benchmark command after --")
    result = args.result.resolve()
    samples_path = result.with_suffix(".steam-telemetry.json")
    if result == samples_path or result.exists() or samples_path.exists():
        parser.error("use a new path for each benchmark result")
    if not result.parent.is_dir():
        parser.error("the result directory does not exist")
    gpu = args.gpu_hwmon
    if gpu is None:
        devices = named_sensors(Path("/sys/class/drm").glob("card[0-9]*/device/hwmon/hwmon*"), "amdgpu")
        if len(devices) != 1:
            parser.error("could not identify one AMDGPU hwmon directory, pass --gpu-hwmon on the host")
        gpu = devices[0]
    cpu = named_sensors(Path("/sys/class/hwmon").glob("hwmon*"), "k10temp")
    cpu_sensor = cpu[0] / "temp1_input" if cpu else None
    limit = read_int(gpu / "temp2_crit")
    samples = [capture(gpu, cpu_sensor)]
    try:
        with subprocess.Popen(command) as process:
            while True:
                try:
                    status = process.wait(timeout=SAMPLE_INTERVAL)
                    break
                except subprocess.TimeoutExpired:
                    samples.append(capture(gpu, cpu_sensor))
    except OSError as error:
        print(f"Could not launch benchmark command: {error}", file=sys.stderr)
        status = 1
    samples.append(capture(gpu, cpu_sensor))
    return record_result(result, samples_path, samples, limit, status)


if __name__ == "__main__":
    sys.exit(main())
