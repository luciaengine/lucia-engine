"""SPIR-V consumer regressions for shader-cook transformations.

Requires spirv-val from the SPIRV-Tools installation used by the shader cook.
"""

from __future__ import annotations

import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import shadercook


# Descriptor IDs share one fragment entry point, ID8.
kSampler = (26, 2)
kSamplerPointer = (32, 3, 0, 2)
kSamplerVariable = (59, 3, 6, 0)
kFloat = (22, 5, 32)
kImage = (25, 4, 5, 1, 0, 0, 0, 1, 0)
kImagePointer = (32, 11, 0, 4)
kImageVariable = (59, 11, 12, 0)


def BuildFragment(declarations: list[tuple[int, ...]]) -> bytes:
    instructions = [
        (17, 1),                              # OpCapability Shader
        (14, 0, 1),                           # OpMemoryModel Logical GLSL450
        (15, 4, 8, 0x6E69616D, 0),             # OpEntryPoint Fragment ID8 "main"
        (16, 8, 7),                           # OpExecutionMode OriginUpperLeft
    ]
    variables = [declaration[2] for declaration in declarations if declaration[0] == 59]
    for binding, variable in enumerate(variables):
        instructions.extend([(71, variable, 34, 0), (71, variable, 33, binding)])
    instructions.extend([(19, 1), (33, 7, 1)])  # OpTypeVoid, OpTypeFunction
    instructions.extend(declarations)
    instructions.extend([
        (54, 1, 8, 0, 7),                     # OpFunction
        (248, 9),                             # OpLabel
        (253,),                               # OpReturn
        (56,),                                # OpFunctionEnd
    ])
    words = [0x07230203, 0x00010000, 0, 16, 0]
    for opcode, *operands in instructions:
        words.extend([((len(operands) + 1) << 16) | opcode, *operands])
    return struct.pack("<" + "I" * len(words), *words)


class DescriptorTypeHoistTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.Validator = shadercook.FindTool("spirv-val", None)
        if cls.Validator is None:
            raise RuntimeError("spirv-val not found; expose the shader cook's SPIRV-Tools installation on PATH")

    def Validate(self, module: bytes):
        with tempfile.TemporaryDirectory(prefix="shadercook-sampler-order-") as directory:
            path = Path(directory) / "fragment.spv"
            path.write_bytes(module)
            result = subprocess.run(
                [str(self.Validator), "--target-env", "vulkan1.1", str(path)],
                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(shadercook.HoistDescriptorTypes(module), module)

    def test_early_sampler_with_late_pointer_is_valid_after_hoisting(self):
        module = BuildFragment([kSampler, kSamplerVariable, kSamplerPointer])
        self.Validate(shadercook.HoistDescriptorTypes(module))

    def test_late_sampler_and_pointer_are_valid_after_hoisting(self):
        module = BuildFragment([kSamplerVariable, kSampler, kSamplerPointer])
        self.Validate(shadercook.HoistDescriptorTypes(module))

    def test_ordered_sampler_definitions_preserve_the_module(self):
        module = BuildFragment([kSampler, kSamplerPointer, kSamplerVariable])
        transformed = shadercook.HoistDescriptorTypes(module)
        self.Validate(transformed)
        self.assertEqual(transformed, module)

    def test_shader_without_samplers_preserves_the_module(self):
        module = BuildFragment([])
        transformed = shadercook.HoistDescriptorTypes(module)
        self.Validate(transformed)
        self.assertEqual(transformed, module)

    def test_image_pointer_is_valid_after_hoisting_without_a_sampler(self):
        module = BuildFragment([kFloat, kImage, kImageVariable, kImagePointer])
        self.Validate(shadercook.HoistDescriptorTypes(module))

    def test_image_pointer_stays_after_its_image_type_with_an_early_sampler_use(self):
        module = BuildFragment([
            kSamplerVariable, kFloat, kImage, kImageVariable,
            kImagePointer, kSampler, kSamplerPointer])
        self.Validate(shadercook.HoistDescriptorTypes(module))

    def test_vector_width_matching_a_sampler_id_does_not_move_definitions(self):
        module = BuildFragment([
            kFloat, (23, 4, 5, 2), kSampler, kSamplerPointer, kSamplerVariable])
        transformed = shadercook.HoistDescriptorTypes(module)
        self.Validate(transformed)
        self.assertEqual(transformed, module)


    def test_interleaved_image_types_preserve_each_pointer_dependency(self):
        module = BuildFragment([
            kFloat, kImage, kImageVariable, (25, 13, 5, 3, 0, 0, 0, 1, 0),
            (59, 14, 15, 0), kImagePointer, (32, 14, 0, 13)])
        self.Validate(shadercook.HoistDescriptorTypes(module))

    def test_variable_before_its_image_type_is_rejected(self):
        module = BuildFragment([kFloat, kImageVariable, kImage, kImagePointer])
        with self.assertRaisesRegex(shadercook.CookError, "precedes its SPIR-V image type"):
            shadercook.HoistDescriptorTypes(module)


    def test_function_type_can_reference_a_sampler_declared_late(self):
        module = BuildFragment([(33, 10, 1, 2), kSampler, kSamplerPointer, kSamplerVariable])
        self.Validate(shadercook.HoistDescriptorTypes(module))


    def test_array_element_can_reference_a_sampler_declared_late(self):
        module = BuildFragment([
            (21, 4, 32, 0), (43, 4, 5, 2), (28, 10, 2, 5),
            kSampler, kSamplerPointer, kSamplerVariable])
        self.Validate(shadercook.HoistDescriptorTypes(module))

    def test_runtime_array_element_can_reference_a_sampler_declared_late(self):
        module = BuildFragment([(29, 10, 2), kSampler, kSamplerPointer, kSamplerVariable])
        self.Validate(shadercook.HoistDescriptorTypes(module))

    def test_undefined_value_can_reference_a_sampler_declared_late(self):
        module = BuildFragment([(1, 2, 10), kSampler, kSamplerPointer, kSamplerVariable])
        self.Validate(shadercook.HoistDescriptorTypes(module))

    def test_function_type_can_reference_an_image_pointer_declared_late(self):
        module = BuildFragment([
            kFloat, kImage, (33, 10, 1, 11), kImageVariable, kImagePointer])
        self.Validate(shadercook.HoistDescriptorTypes(module))


if __name__ == "__main__":
    unittest.main()
