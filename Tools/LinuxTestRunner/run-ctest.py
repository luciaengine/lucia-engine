"""Run CTest registrations that the directly executed Google Test suites did not cover."""

import json
import shutil
import subprocess
import sys
from pathlib import Path


def covered(command, suites, cmake, wrapper):
    if not command:
        return False
    executable = Path(command[0])
    arguments = command[1:]
    if executable.resolve() == cmake and arguments[-2:] == ["-P", str(wrapper)]:
        definitions = dict(arg[2:].split("=", 1) for arg in arguments[:-2]
                           if arg.startswith("-D") and "=" in arg)
        executable = Path(definitions.get("TEST_EXE", "").strip('"'))
        arguments = list(filter(None, definitions.get("TEST_ARGS", "").split(";")))
    return executable in suites and all(arg.startswith("--gtest_filter=")
                                        or arg == "--gtest_also_run_disabled_tests" for arg in arguments)


def main():
    output = Path(sys.argv[1])
    suites = {Path(arg) for arg in sys.argv[2:]}
    cmake = Path(shutil.which("cmake")).resolve()
    wrapper = Path(__file__).resolve().parents[2] / "cmake/RunTestExecutable.cmake"
    ctest = ["ctest", "-C", "DebugFast"]
    registrations = json.loads(subprocess.check_output([*ctest, "--show-only=json-v1"]))
    names = [test["name"] for test in registrations["tests"]
             if not covered(test.get("command", []), suites, cmake, wrapper)]
    if not names:
        print("All registrations were covered by the directly executed suites.")
        return 0
    selected = output / "ctest-tests.txt"
    selected.write_text("".join(name + "\n" for name in names))
    return subprocess.call([*ctest, "--tests-from-file", str(selected), "--output-on-failure",
                            "--timeout", "2400", "--output-junit", str(output / "ctest.xml")])


if __name__ == "__main__":
    sys.exit(main())
