"""Run CTest registrations that the directly executed Google Test suites did not cover."""

from functools import cache
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


@cache
def discovery_targets(directory):
    # GoogleTest.cmake places its target-specific CTest include in the binary
    # directory that called gtest_discover_tests. CMake 4 uses a hash suffix
    # where earlier versions used a numbered invocation in square brackets.
    targets = set()
    for path in directory.glob("*_include.cmake"):
        match = re.fullmatch(r"(.+?)(?:\[\d+\]|_[0-9a-f]+)_include\.cmake", path.name)
        if match:
            targets.add(match.group(1))
    return targets


def covered(test, suites, cmake, wrapper, backtrace):
    command = test.get("command", [])
    properties = {prop["name"]: prop["value"] for prop in test.get("properties", [])}
    if "ENVIRONMENT" in properties or "ENVIRONMENT_MODIFICATION" in properties:
        return False
    working_dir = properties.get("WORKING_DIRECTORY")
    if not command:
        return False
    executable = Path(command[0])
    arguments = command[1:]
    if executable.resolve() == cmake and arguments[-2:] == ["-P", str(wrapper)]:
        definitions = dict(arg[2:].split("=", 1) for arg in arguments[:-2]
                           if arg.startswith("-D") and "=" in arg)
        executable = Path(definitions.get("TEST_EXE", "").strip('"'))
        working_dir = definitions.get("TEST_WORKING_DIR", working_dir)
        # Keep arbitrary CMake lists and escaped arguments under CTest's control.
        wrapper_args = definitions.get("TEST_ARGS", "")
        if ";" in wrapper_args:
            return False
        arguments = [wrapper_args] if wrapper_args else []
    if executable not in suites:
        return False
    if working_dir:
        directory = Path(working_dir.strip('"'))
        # Standard registrations run at the build root, in the suite binary directory,
        # or in the default build subdirectory of the CMake file registering them.
        project = wrapper.parents[1]
        standard = {Path.cwd(), executable.parent}
        index = test.get("backtrace")
        while index is not None:
            node = backtrace["nodes"][index]
            source = Path(backtrace["files"][node["file"]])
            if source.name == "CMakeLists.txt" and source.is_relative_to(project):
                standard.add(Path.cwd() / source.parent.relative_to(project))
                break
            index = node.get("parent")
        if "backtrace" not in test and "--gtest_also_run_disabled_tests" in arguments:
            if executable.name not in discovery_targets(directory):
                return False
        elif directory not in standard:
            return False
    if "--gtest_also_run_disabled_tests" in arguments:
        # GoogleTestAddTests emits this flag even for each ordinary, enabled case.
        # Only an exact, enabled case is already covered by the whole-suite gate.
        filters = [arg.removeprefix("--gtest_filter=") for arg in arguments
                   if arg.startswith("--gtest_filter=")]
        if len(filters) != 1 or any(mark in filters[0] for mark in ("*", "?", ":", "-", "DISABLED_")):
            return False
        arguments = [arg for arg in arguments if arg != "--gtest_also_run_disabled_tests"]
    return all(arg.startswith("--gtest_filter=") for arg in arguments)


def main():
    output = Path(sys.argv[1])
    suites = {Path(arg) for arg in sys.argv[2:]}
    cmake = Path(shutil.which("cmake")).resolve()
    wrapper = Path(__file__).resolve().parents[2] / "cmake/RunTestExecutable.cmake"
    ctest = ["ctest", "-C", "DebugFast"]
    registrations = json.loads(subprocess.check_output([*ctest, "--show-only=json-v1"]))
    selected = [(index, test["name"]) for index, test in enumerate(registrations["tests"], start=1)
                if not covered(test, suites, cmake, wrapper, registrations["backtraceGraph"])]
    if not selected:
        print("All registrations were covered by the directly executed suites.")
        return 0
    (output / "ctest-tests.txt").write_text("".join(name + "\n" for _, name in selected))
    # Test names can repeat across directories. An empty range followed by the exact
    # one-based registration indices selects only these commands.
    indices = "0,0,1," + ",".join(str(index) for index, _ in selected)
    return subprocess.call([*ctest, "-I", indices, "--no-tests=error", "--output-on-failure",
                            "--timeout", "2400", "--output-junit", str(output / "ctest.xml")])


if __name__ == "__main__":
    sys.exit(main())
