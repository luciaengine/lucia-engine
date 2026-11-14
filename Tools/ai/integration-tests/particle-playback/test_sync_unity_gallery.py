"""Offline gallery mesh preparation, without licensed assets or Unity."""
import unittest
import tempfile
from pathlib import Path

from sync_unity_gallery import triangulate_obj_face, sync_render_assets


class ObjFaceTests(unittest.TestCase):
    def test_triangle_is_unchanged(self):
        face = 'f 1/1/1 2/2/1 3/3/1 # triangle'
        self.assertEqual(triangulate_obj_face(face), [face])

    def test_quad_preserves_all_corners_and_winding(self):
        self.assertEqual(triangulate_obj_face('f 1/4/2 2/3/2 3/2/2 4/1/2'), [
            'f 1/4/2 2/3/2 3/2/2', 'f 1/4/2 3/2/2 4/1/2'])

    def test_convex_polygon_preserves_relative_indices(self):
        self.assertEqual(triangulate_obj_face('f -5//1 -4//1 -3//1 -2//1 -1//1'), [
            'f -5//1 -4//1 -3//1', 'f -5//1 -3//1 -2//1', 'f -5//1 -2//1 -1//1'])

    def test_comment_is_not_a_corner(self):
        self.assertEqual(triangulate_obj_face('f 1 2 3 4 # wing'), [
            'f 1 2 3 # wing', 'f 1 3 4 # wing'])

    def test_other_records_are_unchanged(self):
        for line in ('', '# mesh', 'v 1 2 3', 'vt 0.5 0.25', 'vn 0 0 1'):
            with self.subTest(line=line):
                self.assertEqual(triangulate_obj_face(line), [line])

    def test_incomplete_face_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'three corners'):
            triangulate_obj_face('f 1 2')


class MeshResyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.assets = Path(self.temp.name)
        self.scenes = self.assets / 'Scenes'
        self.scenes.mkdir()
        (self.assets / 'models').mkdir()
        self.mesh = self.assets / 'models/vfx_butterfly_00.obj'
        self.original = 'v 0 0 0\nvt 0 0.25\nf 1 2 3 4\n'
        self.mesh.write_text(self.original)

    def sync(self):
        sync_render_assets(self.scenes, self.assets / 'unused-source')

    def test_first_conversion_and_rerun_are_idempotent(self):
        self.sync()
        expected = 'v 0 0 0\nvt 0 0.75\nf 1 2 3\nf 1 3 4\n'
        self.assertEqual(self.mesh.read_text(), expected)
        self.sync()
        self.assertEqual(self.mesh.read_text(), expected)
        self.assertEqual(self.mesh.with_suffix('.obj.before-uv-sync').read_text(), self.original)

    def test_rerun_preserves_later_geometry_and_uv_edits(self):
        self.sync()
        edited = self.mesh.read_text().replace('v 0 0 0', 'v 2 0 0').replace('vt 0 0.75', 'vt 0.4 0.6')
        self.mesh.write_text(edited)
        with self.assertRaisesRegex(ValueError, 'Refusing to overwrite edited mesh'):
            self.sync()
        self.assertEqual(self.mesh.read_text(), edited)
        self.assertEqual(self.mesh.with_suffix('.obj.before-uv-sync').read_text(), self.original)

    def test_existing_original_backup_can_resume_conversion(self):
        self.mesh.with_suffix('.obj.before-uv-sync').write_text(self.original)
        self.sync()
        self.assertIn('vt 0 0.75', self.mesh.read_text())

    def test_beam_resync_also_preserves_edits(self):
        beam = self.assets / 'models/quad_10_0_(0_0,-0_5,0_0)_IndependentAxes.obj'
        beam.write_text('v 0 0 0\n')
        self.sync()
        edited = beam.read_text() + '# edited after conversion\n'
        beam.write_text(edited)
        with self.assertRaisesRegex(ValueError, 'Refusing to overwrite edited mesh'):
            self.sync()
        self.assertEqual(beam.read_text(), edited)


if __name__ == '__main__':
    unittest.main()
