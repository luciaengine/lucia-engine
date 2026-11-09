"""Validate standard-surface sampling through the production WGSL cook."""

from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

import shadercook

kShaders = Path(__file__).resolve().parents[2] / "Engine/Modules/Rendering/Shaders"
kHeaderWords = 5
kOpConstant = 43
kOpConstantComposite = 44
kOpImageSampleExplicitLod = 88
kOpFMul = 133
kOpVectorTimesScalar = 142
kOpDPdx = 207
kOpDPdy = 208
kGradientOperands = 4
kDeclaredProperties = """
struct GE_Props
{
    float alphaCutoff;
    float specularIor;
    vec3 transmissionColor;
    float transmissionWeight;
};
GE_Props Props;
void GE_LoadDeclaredProperties()
{
    Props.alphaCutoff = 0.5;
    Props.specularIor = 1.5;
    Props.transmissionColor = vec3(1.0);
    Props.transmissionWeight = 0.0;
}
"""


def BuildFragment(defines: list[str], bias: float | None = None) -> str:
    source = (kShaders / "Adapters/adapter_forward.glsl").read_text()
    surfaceMarker = "#include GE_SURFACE_SHADER_PATH"
    propertiesMarker = "// GE_DECLARED_PROPERTIES"
    if source.count(surfaceMarker) != 1 or source.count(propertiesMarker) != 1:
        raise RuntimeError("The production adapter's surface/property fixture seam changed")
    source = source.replace(surfaceMarker, '#include "Surfaces/standard_pbr.glsl"')
    source = source.replace(propertiesMarker, kDeclaredProperties)
    version, body = source.split("\n", 1)
    source = version + "\n" + "\n".join("#define " + define for define in defines) + "\n" + body
    if bias is not None:
        marker = '#include "../Includes/view_params.glsl"'
        if source.count(marker) != 1:
            raise RuntimeError("The production adapter's view fixture seam changed")
        source = source.replace(marker, marker + "\n#undef ge_mipBiasParams\n"
                                + f"#define ge_mipBiasParams vec4({bias}, 0.0, 0.0, 0.0)")
    return source


def GradientScales(module: bytes) -> list[tuple[float, ...]]:
    words = struct.unpack("<" + "I" * (len(module) // 4), module)
    definitions = {}
    gradients = []
    index = kHeaderWords
    while index < len(words):
        count, opcode = words[index] >> 16, words[index] & 0xFFFF
        operands = words[index + 1:index + count]
        if opcode in (kOpConstant, kOpConstantComposite, kOpFMul, kOpVectorTimesScalar, kOpDPdx, kOpDPdy):
            definitions[operands[1]] = (opcode, operands)
        if opcode == kOpImageSampleExplicitLod and operands[4] == kGradientOperands:
            gradients.extend(operands[5:7])
        index += count

    def Constant(identifier: int) -> tuple[float, ...] | None:
        definition = definitions.get(identifier)
        if definition is None:
            return None
        opcode, operands = definition
        if opcode == kOpConstant:
            return (struct.unpack("<f", struct.pack("<I", operands[2]))[0],)
        if opcode == kOpConstantComposite:
            components = [Constant(value) for value in operands[2:]]
            if all(component is not None for component in components):
                return tuple(value for component in components for value in component)
        return None

    def Scale(identifier: int) -> tuple[float, ...]:
        definition = definitions.get(identifier)
        if definition is None:
            raise AssertionError("Unrecognized explicit gradient producer")
        opcode, operands = definition
        if opcode in (kOpDPdx, kOpDPdy):
            return (1.0,)
        if opcode not in (kOpFMul, kOpVectorTimesScalar):
            raise AssertionError("Unexpected gradient arithmetic in the sampler bias consumer")
        factors = [Constant(value) for value in operands[2:]]
        if sum(factor is not None for factor in factors) != 1:
            raise AssertionError("The gradient must have one observable constant bias multiplier")
        values = [factor if factor is not None else Scale(identifier)
                  for identifier, factor in zip(operands[2:], factors)]
        width = max(len(value) for value in values)
        return tuple(values[0][index % len(values[0])] * values[1][index % len(values[1])]
                     for index in range(width))

    return [Scale(gradient) for gradient in gradients]



class CompatHexSamplingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.Tools = {name: shadercook.FindTool(name, None)
                     for name in ("glslc", "naga", "tint")}
        missing = [name for name, path in cls.Tools.items() if path is None]
        if missing:
            raise RuntimeError("Expose shader-cook tools on PATH: " + ", ".join(missing))

    def CookFragment(self, defines: list[str], bias: float | None = None) -> bytes:
        common = ["GE_STAGE_FRAGMENT", "GE_COMPAT_PROFILE", "HAS_POSITION", "HAS_NORMAL",
                  "HAS_UV0", "LIGHTING_MODEL_STANDARD_PBR"]
        with tempfile.TemporaryDirectory(prefix="shadercook-compat-hex-") as directory:
            root = Path(directory)
            source = root / "standard.frag"
            source.write_text(BuildFragment(common + defines, bias))
            shadercook.CookFile(source, "fragment", self.Tools["glslc"], self.Tools["naga"],
                               self.Tools["tint"], True, [],
                               [kShaders / "Adapters", kShaders, kShaders / "Includes"],
                               root / "standard.wgsl", root)
            return (root / "standard.spv").read_bytes()

    def test_opaque_runtime_hex_color_sampling_passes_wgsl_validation(self):
        self.CookFragment([])

    def test_masked_runtime_hex_normal_sampling_passes_wgsl_validation(self):
        self.CookFragment(["ALPHA_TEST", "HAS_TANGENT"])

    def test_blended_runtime_hex_normal_sampling_passes_wgsl_validation(self):
        self.CookFragment(["GE_ALPHA_BLEND", "HAS_TANGENT"])

    def test_runtime_hex_coat_normal_sampling_passes_wgsl_validation(self):
        self.CookFragment(["HAS_TANGENT", "GE_COAT_NORMAL_ENABLED"])

    def CheckBiasScale(self, bias: float):
        scales = GradientScales(self.CookFragment([], bias))
        self.assertGreaterEqual(len(scales), 8)
        for scale in scales:
            self.assertTrue(all(value == 2.0 ** bias for value in scale), scale)

    def test_second_tap_gradient_has_unit_scale_at_zero_view_bias(self):
        self.CheckBiasScale(0.0)

    def test_second_tap_gradient_has_one_nonzero_view_bias_scale(self):
        self.CheckBiasScale(1.0)


if __name__ == "__main__":
    unittest.main()
