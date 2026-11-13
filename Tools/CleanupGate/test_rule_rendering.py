"""Tests the rules of 08_rendering over the fixtures in Fixtures/: the line of every site each pattern counts."""
import os
import sys
import unittest

sys.dont_write_bytecode = True
kGateFolder = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, kGateFolder)

import gate_common  # noqa: E402
import rule_rendering  # noqa: E402


def site_lines(fixture, pattern):
    source = gate_common.load_source(os.path.join(kGateFolder, "Fixtures"), fixture)
    return [site.line for site in rule_rendering.Rule([]).scan_file(source) if site.pattern == pattern]


class RenderingRuleTests(unittest.TestCase):
    def test_create_texture_counts_device_calls_and_not_render_graph_frame_calls(self):
        self.assertEqual(site_lines("create_texture.cpp", "create_texture"), [6, 7, 8])


if __name__ == "__main__":
    unittest.main()
