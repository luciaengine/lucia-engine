"""Exercise the staged authoring tool without an editor or graphics device."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


parser = argparse.ArgumentParser()
parser.add_argument("--tool", required=True)
arguments, remaining = parser.parse_known_args()
TOOL = str(Path(arguments.tool).resolve())


class MeshClusterCookCliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mesh-cluster-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "grid.obj"
        rows = ["o grid", "vn 0 0 1"]
        side = 17
        for y in range(side):
            for x in range(side):
                rows.append(f"v {x} {y} 0")
        for y in range(side - 1):
            for x in range(side - 1):
                a = y * side + x + 1
                b, c, d = a + 1, a + side, a + side + 1
                rows.extend((f"f {a}//1 {b}//1 {d}//1", f"f {a}//1 {d}//1 {c}//1"))
        self.source.write_text("\n".join(rows) + "\n")

    def run_tool(self, *arguments):
        return subprocess.run([TOOL, *map(str, arguments)], cwd=self.root,
                              capture_output=True, text=True, timeout=60)

    def test_cooks_deterministically_without_modifying_source(self):
        original = self.source.read_bytes()
        first, second = self.root / "first.gecluster", self.root / "second.gecluster"
        for output in (first, second):
            result = self.run_tool("--input", self.source, "--output", output)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Cluster groups:", result.stdout)
            self.assertTrue(output.is_file())
        self.assertEqual(first.read_bytes(), second.read_bytes())
        stores = list((self.root / ".cluster").glob("*.pages"))
        self.assertEqual(len(stores), 1)
        self.assertGreater(stores[0].stat().st_size, 0)
        self.assertEqual(self.source.read_bytes(), original)

    def test_refuses_existing_output_without_touching_it(self):
        output = self.root / "existing.gecluster"
        output.write_bytes(b"previous authored output")
        before = hashlib.sha256(output.read_bytes()).hexdigest()
        result = self.run_tool("--input", self.source, "--output", output)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), before)
        self.assertFalse((self.root / ".cluster").exists())

    def test_cooks_static_gltf_with_a_shared_mesh_hierarchy(self):
        source, output = self.root / "static.gltf", self.root / "static.gecluster"
        document = {
            "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
            "nodes": [{"translation": [3, 2, 5], "children": [1, 2]},
                      {"mesh": 0, "translation": [7, 0, 1]}, {"mesh": 0, "translation": [-4, 0, 2]}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
            "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3,
                           "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 1]}],
            "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
            "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,"
                         "AACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}],
        }
        source.write_text(json.dumps(document))
        original = source.read_bytes()
        result = self.run_tool("--input", source, "--output", output)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(output.is_file())
        self.assertEqual(source.read_bytes(), original)

    def test_refuses_cluster_recook_without_publishing_output(self):
        output, recook = self.root / "first.gecluster", self.root / "recook.gecluster"
        result = self.run_tool("--input", self.source, "--output", output)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.run_tool("--input", output, "--output", recook)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(recook.exists())

    def test_refuses_missing_manifest_and_malformed_arguments(self):
        output = self.root / "missing.gecluster"
        for arguments in (("--input", self.source, "--output", output, "--assets", self.root),
                          ("--input", self.source),
                          ("--input", self.source, "--output", output, "--unknown", "value")):
            result = self.run_tool(*arguments)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(argv=[__file__, *remaining])
