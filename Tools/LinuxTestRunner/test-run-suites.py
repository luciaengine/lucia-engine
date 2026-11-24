"""Exercise the Linux gate with a real CTest project and a recording suite."""

import shutil
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
if [ "$1" = "--gtest_list_tests" ]; then
  printf 'One.\\n  Test\\n'
  exit 0
fi
echo "${1%%=*}" >> "$0.runs"
case "$1" in
  --gtest_output=xml:*) echo '<testsuites tests="1" disabled="0" failures="0" errors="0"><testcase status="run"/></testsuites>' > "${1#--gtest_output=xml:}";;
esac
""")
            suite.chmod(0o755)
            runner = root / "Tools/LinuxTestRunner"
            shutil.copytree(RUNNER, runner)
            wrapper = root / "cmake/RunTestExecutable.cmake"
            wrapper.parent.mkdir()
            shutil.copy(RUNNER.parents[1] / "cmake/RunTestExecutable.cmake", wrapper)
            (root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.25)
project(GateFixture C)
enable_testing()
add_subdirectory(Module)
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
            module = root / "Module"
            module.mkdir()
            (module / "unused.c").write_text("// TEST(One, Static)\nint main(void) { return 0; }")
            (module / "CMakeLists.txt").write_text(f"""
include(GoogleTest)
add_test(NAME DirectAlias COMMAND "${{CMAKE_COMMAND}}" -E true)
add_executable(ExampleTests unused.c)
set_target_properties(ExampleTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "{suite.parent}")
gtest_add_tests(TARGET ExampleTests SOURCES unused.c)
gtest_discover_tests(ExampleTests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(ExampleTests DISCOVERY_MODE PRE_TEST TEST_PREFIX Alternate.
                     WORKING_DIRECTORY "{build}")
""")
            subprocess.run(["cmake", "-S", root, "-B", build], check=True, capture_output=True)
            runs = Path(str(suite) + ".runs")
            for failing in (True, False):
                with self.subTest(failing=failing):
                    output = root / str(failing)
                    (output / "xml").mkdir(parents=True)
                    (root / "cook.cmake").write_text('message(FATAL_ERROR "fixture")' if failing else '')
                    runs.write_text("")
                    result = subprocess.run(["bash", runner / "run-suites.sh", output], cwd=build)
                    self.assertEqual(result.returncode, int(failing))
                    self.assertCountEqual(runs.read_text().splitlines(), ["--gtest_output", "--gtest_also_run_disabled_tests",
                                                             "--gtest_filter", "--gtest_filter", "--integration",
                                                             "--gtest_filter"])
                    selected = (output / "ctest-tests.txt").read_text().splitlines()
                    self.assertNotIn("One.Test", selected)
                    self.assertNotIn("One.Static", selected)
                    self.assertIn("Alternate.One.Test", selected)
                    self.assertIn("Cooker CLI", (output / "ctest.log").read_text())
                    self.assertTrue((output / "ctest.xml").is_file())


if __name__ == "__main__":
    unittest.main()
