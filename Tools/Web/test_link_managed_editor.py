"""Verify the managed editor's deferred shader preload."""
import json
import tempfile
import unittest
from pathlib import Path

import link_managed_editor as linker


class EditorPreloadTests(unittest.TestCase):
    def test_split_keeps_ordered_bytes_in_each_preload(self):
        metadata = {
            "files": [
                {"filename": "/.Cache/Shaders/a/program.shaderpkg", "start": 0, "end": 2},
                {"filename": "/Assets/UI/a.css", "start": 2e0, "end": 5},
                {"filename": "/.Cache/Shaders/index.bin", "start": 5, "end": 8},
                {"filename": "/SeedProject/WebSmoke/test", "start": 8, "end": 9},
            ],
            "remote_package_size": 9,
        }
        source = f"  loadPackage({json.dumps(metadata)});\n}}"
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            archive = directory / "sdk.data"
            archive.write_bytes(b"abCSSIDXZ")
            rewritten = linker.split_editor_data(source, archive, directory)
            self.assertEqual(archive.read_bytes(), b"abCSSIDXZ")
            self.assertEqual((directory / "dotnet.native.data").read_bytes(), b"CSSZ")
            self.assertEqual((directory / "dotnet.native.shaders.data").read_bytes(), b"abIDX")

        decoder = json.JSONDecoder()
        calls = rewritten.splitlines()
        startup, _ = decoder.raw_decode(calls[0].removeprefix("  loadPackage("))
        shaders, end = decoder.raw_decode(calls[1].removeprefix("  loadPackage("))
        self.assertEqual(calls[1].removeprefix("  loadPackage(")[end:], ', "dotnet.native.shaders.data");')
        self.assertEqual([(f["start"], f["end"]) for f in startup["files"]], [(0, 3), (3, 4)])
        self.assertEqual([(f["start"], f["end"]) for f in shaders["files"]], [(0, 2), (2, 5)])
        self.assertEqual(startup["remote_package_size"], 4)
        self.assertEqual(shaders["remote_package_size"], 5)

    def test_refuses_wrong_archive_size(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            archive = directory / "sdk.data"
            archive.write_bytes(b"wrong")
            with self.assertRaisesRegex(ValueError, "does not match the data file"):
                linker.split_editor_data(
                    '  loadPackage({"files":[],"remote_package_size":1});', archive, directory)
            self.assertEqual(list(directory.iterdir()), [archive])


if __name__ == "__main__":
    unittest.main()
