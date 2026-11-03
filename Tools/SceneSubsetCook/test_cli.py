import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


class SceneSubsetCookTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.scene = self.root/'authored.scene'
        self.scene.write_text('[scene name="Test" version=1]\n'
            '[entity id="child" parent="root"]\nName.value="Destination"\nSkyEnvironment.SunLight="sun"\n'
            '[entity id="unused"]\nName.value="Other level"\n'
            '[entity id="sun"]\nLight.intensity=2\n'
            '[entity id="root"]\nName.value="Shared parent"\n')
        self.identity = hashlib.sha256(self.scene.read_bytes()).hexdigest()
        self.output = self.root/'derived'
        self.selection = self.root/'roots.json'

    def run_tool(self, roots=None, success=True, output=None):
        command = [TOOL, '--scene', str(self.scene), '--output', str(output or self.output)]
        if roots is not None:
            self.selection.write_text(json.dumps(roots))
            command += ['--selection', str(self.selection)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        self.assertEqual(hashlib.sha256(self.scene.read_bytes()).hexdigest(), self.identity)
        return result

    def test_inspection_keeps_source_and_does_not_emit_replacement_scene(self):
        self.run_tool()
        catalog = json.loads((self.output/'catalog.json').read_text())
        self.assertEqual(len(catalog['entities']), 4)
        self.assertEqual(catalog['entities'][0]['dependency_ids'], ['root', 'sun'])
        self.assertFalse(catalog['semantic_dependencies_inferred'])
        self.assertFalse((self.output/'scene.scene').exists())

    def test_subset_closure_and_roundtrip(self):
        self.run_tool(['child'])
        catalog = json.loads((self.output/'catalog.json').read_text())
        self.assertEqual([e['id'] for e in catalog['entities']], ['child', 'sun', 'root'])
        self.assertEqual(json.loads((self.output/'complete.json').read_text())['entities'], 3)
        self.scene = self.output/'scene.scene'
        self.identity = hashlib.sha256(self.scene.read_bytes()).hexdigest()
        self.run_tool(output=self.root/'roundtrip')
        actual = json.loads((self.root/'roundtrip/catalog.json').read_text())
        self.assertEqual(actual['entities'], catalog['entities'])

    def test_unknown_root_and_invalid_selection_fail_before_output(self):
        for roots in (['missing'], {'id': 'child'}, [False]):
            with self.subTest(roots=roots):
                self.run_tool(roots, success=False)
                self.assertFalse(self.output.exists())

    def test_empty_subset_is_explicit(self):
        self.run_tool([])
        self.assertEqual(json.loads((self.output/'catalog.json').read_text())['entities'], [])

    def test_existing_output_and_authored_directory_are_preserved(self):
        self.output.mkdir()
        sentinel = self.output/'keep'
        sentinel.write_text('keep')
        self.run_tool(['child'], success=False)
        self.run_tool(['child'], success=False, output=self.root)
        self.assertEqual(sentinel.read_text(), 'keep')
        self.assertEqual(list(self.output.iterdir()), [sentinel])

    def test_linked_destination_is_rejected(self):
        target = self.root/'owned'
        target.mkdir()
        link = self.root/'linked'
        link.symlink_to(target, target_is_directory=True)
        self.run_tool(['child'], success=False, output=link/'derived')
        self.assertFalse((target/'derived').exists())

    def test_unparseable_values_and_embeds_fail_closed(self):
        for body in ('[entity id="a"]\nFuture.Value=(broken\n',
                     '[embed id="mat" type="Material"]\nalbedo=(1,0,0)\n'):
            self.scene.write_text('[scene name="Unsupported" version=1]\n'+body)
            self.identity = hashlib.sha256(self.scene.read_bytes()).hexdigest()
            self.run_tool(success=False)
            self.assertFalse(self.output.exists())

    def test_batch_closes_each_group_and_preserves_input(self):
        groups = self.root/'groups.json'
        groups.write_text(json.dumps({'first': ['child'], 'second': ['unused'], 'empty': []}))
        result = subprocess.run([TOOL, '--scene', str(self.scene), '--output', str(self.output),
                                 '--groups', str(groups)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads((self.output/'complete.json').read_text())['groups']
        self.assertEqual(report['first']['entity_ids'], ['child', 'sun', 'root'])
        self.assertEqual(report['second']['entity_ids'], ['unused'])
        self.assertEqual(report['empty']['entity_ids'], [])
        self.assertEqual(hashlib.sha256(self.scene.read_bytes()).hexdigest(), self.identity)
        for record in report.values():
            self.assertEqual((self.output/record['file']).stat().st_size, record['bytes'])

    def test_invalid_batch_rejected_without_output(self):
        groups = self.root/'groups.json'
        for data in ({'ok': ['child'], 'bad': ['missing']}, {'../escape': ['child']},
                     {'CON': []}, {'nul': []}, {'a.scene': []}, {'a': False}, []):
            groups.write_text(json.dumps(data))
            result = subprocess.run([TOOL, '--scene', str(self.scene), '--output', str(self.output),
                                    '--groups', str(groups)], capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(self.output.exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--tool', required=True)
    args, remaining = parser.parse_known_args()
    TOOL = str(Path(args.tool).resolve())
    unittest.main(argv=[__file__, *remaining])
