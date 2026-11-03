"""Exercise the real headless cooker, not a mock of its artifact contract."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--tool', type=Path, required=True)
options, remaining = parser.parse_known_args()
TOOL = options.tool.resolve()
GUID = 'e655b4ed-28a1-4d3f-88e4-d30edcc1f140'


class CookCliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='texture-cook-cli-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def stage(self, mount=None, path='leaf.tga', **metadata):
        mount = mount or self.root
        assets = mount/'Assets'
        assets.mkdir(parents=True, exist_ok=True)
        header = bytearray(18)
        header[2], header[12], header[14], header[16], header[17] = 2, 8, 8, 32, 0x28
        (assets/'leaf.tga').write_bytes(header + bytes([180, 120, 80, 255])*64)
        record = {'guid': GUID, 'type': 'Texture', 'path': path,
                  'kv': {'assets.texture.usage': 'color',
                         'assets.texture.compression': 'none', **metadata}}
        (assets/'.assetmanifest').write_text(json.dumps(record)+'\n')
        return assets

    def cook(self, success=True):
        result = subprocess.run([str(TOOL), str(self.root)], text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, success, result.stdout+result.stderr)
        return result.stdout+result.stderr

    def test_cooks_and_reuses_from_fresh_staged_inputs(self):
        assets = self.stage()
        self.assertIn('1 artifacts (0 reused)', self.cook())
        artifacts = list((self.root/'Tex').glob('*.ktx2'))
        self.assertEqual(len(artifacts), 1)
        self.assertTrue(artifacts[0].read_bytes().startswith(b'\xabKTX 20\xbb\r\n\x1a\n'))
        first = artifacts[0].read_bytes()
        self.assertFalse((assets/'leaf.tga').exists())
        manifest = next(json.loads(line) for line in (assets/'.assetmanifest').read_text().splitlines()
                        if line.startswith('{') and json.loads(line).get('guid') == GUID)
        self.assertEqual(manifest['kv']['assets.texture.packaged'], artifacts[0].name)
        self.stage()
        self.assertIn('1 artifacts (1 reused)', self.cook())
        self.assertEqual(artifacts[0].read_bytes(), first)
        self.assertFalse((assets/'leaf.tga').exists())

    def test_cooks_package_into_its_own_mount(self):
        (self.root/'Assets').mkdir()
        (self.root/'Assets/.assetmanifest').write_text('')
        package = self.root/'Packages/scenery'
        self.stage(package)
        self.cook()
        self.assertEqual(len(list((package/'Tex').glob('*.ktx2'))), 1)
        self.assertFalse((self.root/'Tex').exists())

    def test_valid_staged_artifact_is_not_republished(self):
        self.stage()
        self.cook()
        artifact, = (self.root/'Tex').glob('*.ktx2')
        old_time = 1600000000000000000
        os.utime(artifact, ns=(old_time, old_time))
        before = artifact.stat()
        self.stage()
        self.assertIn('1 artifacts (1 reused)', self.cook())
        after = artifact.stat()
        self.assertEqual(after.st_mtime_ns, before.st_mtime_ns)
        self.assertEqual(after.st_ino, before.st_ino)

    def test_rejects_missing_manifest(self):
        self.cook(success=False)

    def test_rejects_missing_texture(self):
        assets = self.stage()
        (assets/'leaf.tga').unlink()
        self.assertIn('Could not read staged texture', self.cook(success=False))

    def test_rejects_escaping_path(self):
        self.stage(path='../leaf.tga')
        self.assertIn('escapes asset root', self.cook(success=False))

    def test_rejects_invalid_import_policy(self):
        self.stage(**{'assets.texture.alphaCoverage': 'invalid'})
        self.assertIn('Invalid texture import policy', self.cook(success=False))


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
