"""Exercise the Linux gate with a real CTest project and a recording suite."""

import subprocess
import tempfile
import unittest
from pathlib import Path


RUNNER = Path(__file__).resolve().parent


class LinuxGateTests(unittest.TestCase):
    def test_registered_commands_and_suite_aliases(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            build = root / "build"
            suite = build / "bin/DebugFast/Tests/ExampleTests"
            suite.parent.mkdir(parents=True)
            suite.write_text("""#!/bin/bash
echo "${1%%=*}" >> "$0.runs"
case "$1" in
  --gtest_output=xml:*) echo '<testsuites tests="1" disabled="0" failures="0" errors="0"><testcase status="run"/></testsuites>' > "${1#--gtest_output=xml:}";;
esac
""")
            suite.chmod(0o755)
            wrapper = RUNNER.parents[1] / "cmake/RunTestExecutable.cmake"
            (root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.25)
project(GateFixture NONE)
enable_testing()
add_test(NAME DirectAlias COMMAND "{suite}" --gtest_filter=One.Test --gtest_also_run_disabled_tests)
add_test(NAME DisabledOptIn COMMAND "{suite}" --gtest_also_run_disabled_tests)
add_test(NAME DescriptorMode COMMAND "{suite}" --gtest_filter=Mode.Test)
set_tests_properties(DescriptorMode PROPERTIES ENVIRONMENT "GE_VK_USE_DESCRIPTOR_BUFFER=0")
add_test(NAME CustomDirectory COMMAND "{suite}" --gtest_filter=Path.Test)
set_tests_properties(CustomDirectory PROPERTIES WORKING_DIRECTORY "{root}")
add_test(NAME WrappedAlias COMMAND "${{CMAKE_COMMAND}}" "-DTEST_EXE={suite}"
         "-DTEST_ARGS=--gtest_filter=One.Test" -P "{wrapper}")
add_test(NAME CustomMode COMMAND "{suite}" --integration)
add_test(NAME "Cooker CLI" COMMAND "${{CMAKE_COMMAND}}" -P "{root}/cook.cmake")
add_test(NAME Disabled COMMAND "${{CMAKE_COMMAND}}" -E false)
set_tests_properties(Disabled PROPERTIES DISABLED TRUE)
""")
            subprocess.run(["cmake", "-S", root, "-B", build], check=True, capture_output=True)
            runs = Path(str(suite) + ".runs")
            for failing in (True, False):
                with self.subTest(failing=failing):
                    output = root / str(failing)
                    (output / "xml").mkdir(parents=True)
                    (root / "cook.cmake").write_text('message(FATAL_ERROR "fixture")' if failing else '')
                    runs.write_text("")
                    result = subprocess.run(["bash", RUNNER / "run-suites.sh", output], cwd=build)
                    self.assertEqual(result.returncode, int(failing))
                    self.assertEqual(runs.read_text().splitlines(), ["--gtest_output", "--gtest_also_run_disabled_tests",
                                                             "--gtest_filter", "--gtest_filter", "--integration"])
                    self.assertIn("Cooker CLI", (output / "ctest.log").read_text())
                    self.assertTrue((output / "ctest.xml").is_file())


if __name__ == "__main__":
    unittest.main()
