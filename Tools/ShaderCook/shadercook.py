#!/usr/bin/env python3
"""GLSL -> SPIR-V(1.1) -> WGSL cook for the WebGPU backend.

Pipeline (each stage's constraint is load-bearing, see the web platform plan):
  glslc  --target-env=vulkan1.1   naga rejects SPIR-V 1.4+ (OpCopyLogical),
                                  and a 1.3 pack panics wgpu-native.
  naga   --keep-coordinate-space  without it naga bakes a clip-space Y flip
                                  the SPIR-V path does not have.
  tint                            Chrome/Dawn conformance oracle; errors on
                                  uniformity violations naga only warns on.
                                  Missing tint fails the cook unless the
                                  caller opts out explicitly.

The naga under toolchain/<host>/ is version-matched to the engine's wgpu-native
port (see toolchain/manifest.json); translator and desktop validator are the
same code. Provision both tools with toolchain/setup.py.
"""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

kStageByExtension = {
    ".vert": "vertex",
    ".frag": "fragment",
    ".comp": "compute",
}

kSelfTestShader = """#version 450
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) buffer OutBuf { uint values[]; };
void main() { values[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.x * 3u + 7u; }
"""


def HostToolchainDir() -> Path:
    machine = platform.machine().lower()
    if sys.platform == "darwin":
        host = "macos-arm64" if machine in ("arm64", "aarch64") else "macos-x64"
    elif sys.platform.startswith("linux"):
        host = "linux-x64"
    else:
        host = "windows-x64"
    return Path(__file__).resolve().parent / "toolchain" / host


def FindTool(name: str, explicit: str | None) -> Path | None:
    if explicit:
        path = Path(explicit)
        return path if path.exists() else None
    vendored = HostToolchainDir() / (name + (".exe" if sys.platform == "win32" else ""))
    if vendored.exists():
        return vendored
    found = shutil.which(name)
    return Path(found) if found else None


def Run(argv: list[str | Path]) -> subprocess.CompletedProcess:
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True)


# WebGPU/WGSL has no combined image samplers, and naga's SPIR-V frontend
# rejects the OpImage-of-combined-load pattern GLSL's textureSize() produces.
# spirv-opt --split-combined-image-sampler separates them but leaves the new
# sampler on the IMAGE's binding slot; the cook moves exactly those samplers to
# kSamplerBindingOffset + original so the pair coexists. The backend builds its
# bind group layouts with the same rule
# (Engine::Renderer::MaterialBindingCache::kSamplerBindingOffset).
#
# A sampler the SHADER declared separately is already in a slot of its own and
# must be left alone: the compat material set declares its samplers at
# binding+64 itself, and renumbering those again would land them at 128 —
# bindings no layout builds. "Created by the split" is exactly "shares a
# (set, binding) with an image variable", so that collision is the test.
kSamplerBindingOffset = 64

kSpirvHeaderWordCount = 5
kStorageClassUniformConstant = 0

kOpUndef = 1
kOpTypeImage = 25
kOpTypeSampler = 26
kOpTypeSampledImage = 27
kOpTypeArray = 28
kOpTypeRuntimeArray = 29
kOpTypeStruct = 30
kOpTypePointer = 32
kOpTypeFunction = 33
kOpVariable = 59
kOpDecorate = 71
kDecorationBinding = 33
kDecorationDescriptorSet = 34

def HoistDescriptorTypes(spv: bytes) -> bytes:
    """Place split descriptor definitions before their type uses.

    Split declarations can precede a reused sampler or pointer type.
    Move each required definition separately, after its own dependencies.
    """
    words = memoryview(spv).cast("I")
    total = len(words)
    instructions: list[tuple[int, int, int]] = []
    index = kSpirvHeaderWordCount
    minimumCounts = {kOpTypeImage: 9, kOpTypeSampler: 2, kOpTypeArray: 4,
                     kOpTypeRuntimeArray: 3, kOpTypeStruct: 2, kOpTypeFunction: 3,
                     kOpTypePointer: 4, kOpVariable: 4, kOpUndef: 3}
    while index < total:
        count = words[index] >> 16
        opcode = words[index] & 0xFFFF
        if count < minimumCounts.get(opcode, 1) or index + count > total:
            raise CookError("Invalid SPIR-V instruction in descriptor type repair")
        instructions.append((index, count, opcode))
        index += count

    imageTypes = {words[start + 1] for start, _, opcode in instructions
                  if opcode == kOpTypeImage}
    samplerTypes = {words[start + 1] for start, _, opcode in instructions
                    if opcode == kOpTypeSampler}
    descriptorTypes = imageTypes | samplerTypes
    definitions = {}
    for instruction in instructions:
        start, _, opcode = instruction
        if opcode == kOpTypeSampler or (
                opcode == kOpTypePointer and words[start + 2] == kStorageClassUniformConstant
                and words[start + 3] in descriptorTypes):
            definitions[words[start + 1]] = instruction
    if not definitions:
        return spv

    emitted = set()
    emittedImages = set()
    out = bytearray(spv[:kSpirvHeaderWordCount * 4])

    def EmitDefinition(typeId: int) -> None:
        if typeId not in definitions or typeId in emitted:
            return
        start, count, opcode = definitions[typeId]
        if opcode == kOpTypePointer:
            pointee = words[start + 3]
            if pointee in imageTypes and pointee not in emittedImages:
                raise CookError("Descriptor use precedes its SPIR-V image type")
            EmitDefinition(pointee)
        out.extend(spv[start * 4:(start + count) * 4])
        emitted.add(typeId)

    for start, count, opcode in instructions:
        if opcode in (kOpTypeFunction, kOpTypeStruct):
            for offset in range(2, count):
                EmitDefinition(words[start + offset])
        elif opcode in (kOpTypeArray, kOpTypeRuntimeArray):
            EmitDefinition(words[start + 2])
        elif opcode == kOpTypePointer:
            EmitDefinition(words[start + 3])
        elif opcode in (kOpVariable, kOpUndef):
            EmitDefinition(words[start + 1])
        if opcode in (kOpTypeSampler, kOpTypePointer) and words[start + 1] in definitions:
            EmitDefinition(words[start + 1])
            continue
        if opcode == kOpTypeImage:
            emittedImages.add(words[start + 1])
        out.extend(spv[start * 4:(start + count) * 4])
    return bytes(out)


def RenumberSamplerBindings(spv: bytes) -> tuple[bytes, list[dict]]:
    words = bytearray(spv)

    def ReadWord(index: int) -> int:
        return int.from_bytes(words[index * 4:(index + 1) * 4], "little")

    def WriteWord(index: int, value: int) -> None:
        words[index * 4:(index + 1) * 4] = value.to_bytes(4, "little")

    sampler_types: set[int] = set()
    image_types: set[int] = set()
    sampler_pointer_types: set[int] = set()
    image_pointer_types: set[int] = set()
    sampler_variables: set[int] = set()
    image_variables: set[int] = set()
    binding_decorations: dict[int, int] = {}  # target id -> word index of value
    set_decorations: dict[int, int] = {}      # target id -> word index of value

    index = 5  # past the SPIR-V header
    total = len(words) // 4
    while index < total:
        first = ReadWord(index)
        opcode = first & 0xFFFF
        count = first >> 16
        if count == 0:
            break
        if opcode == kOpTypeSampler:
            sampler_types.add(ReadWord(index + 1))
        elif opcode in (kOpTypeImage, kOpTypeSampledImage):
            image_types.add(ReadWord(index + 1))
        elif opcode == kOpTypePointer and count >= 4:
            pointee = ReadWord(index + 3)
            if pointee in sampler_types:
                sampler_pointer_types.add(ReadWord(index + 1))
            elif pointee in image_types:
                image_pointer_types.add(ReadWord(index + 1))
        elif opcode == kOpVariable and count >= 4:
            pointer_type = ReadWord(index + 1)
            if pointer_type in sampler_pointer_types:
                sampler_variables.add(ReadWord(index + 2))
            elif pointer_type in image_pointer_types:
                image_variables.add(ReadWord(index + 2))
        elif opcode == kOpDecorate and count >= 4:
            decoration = ReadWord(index + 2)
            if decoration == kDecorationBinding:
                binding_decorations[ReadWord(index + 1)] = index + 3
            elif decoration == kDecorationDescriptorSet:
                set_decorations[ReadWord(index + 1)] = index + 3
        index += count

    def SlotOf(variable: int) -> tuple[int, int] | None:
        value_index = binding_decorations.get(variable)
        if value_index is None:
            return None
        set_index = set_decorations.get(variable)
        return (ReadWord(set_index) if set_index is not None else 0, ReadWord(value_index))

    image_slots = {slot for slot in (SlotOf(v) for v in image_variables) if slot is not None}

    remap = []
    for variable in sampler_variables:
        slot = SlotOf(variable)
        if slot is None or slot not in image_slots:
            continue
        value_index = binding_decorations[variable]
        original = ReadWord(value_index)
        WriteWord(value_index, original + kSamplerBindingOffset)
        remap.append({"set": slot[0],
                      "imageBinding": original,
                      "samplerBinding": original + kSamplerBindingOffset})
    return bytes(words), remap


class CookError(RuntimeError):
    pass


# Browsers reject var<push_constant>; the cook rewrites the block to a UBO at
# a reserved bind group (WebGPU allows 4 groups; the engine's material layout
# uses sets 0-2). The backend emulates SetPushConstants by binding a fresh
# uniform buffer at this group per touching draw. NOTE: the rewrite changes
# the block's layout rules from std430 to std140 — identical for the
# scalar/vecN members the engine's push blocks use today, but an array or
# vec3 member would need explicit std430/scalar layout instead.
kPushConstantGroup = 3
kPushConstantRewrite = f"layout(set = {kPushConstantGroup}, binding = 0)"


def RewritePushConstants(text: str) -> str:
    import re as _re
    return _re.sub(r"layout\s*\(\s*push_constant\s*\)", kPushConstantRewrite, text)


def Preprocess(source: Path, stage: str, glslc: Path, defines: list[str],
               include_dirs: list[Path], out: Path) -> None:
    """Flatten the include closure into one translation unit.

    The push-constant rewrite is textual, so it can only see declarations in
    text it holds — and the engine declares push-constant blocks inside
    includes (instance_io.glsl, sssr_common.glsl), not only at top level.
    Flattening first is what makes the rewrite reach them. glslc compiles its
    own -E output to byte-identical SPIR-V, so this costs nothing but a pass.
    """
    cmd = [glslc, "-E", f"-fshader-stage={stage}"]
    # Web pipelines keep runtime feature branches; native-only overrides use their defaults.
    cmd.append("-DGE_WGSL_COOK=1")
    for define in defines:
        cmd.append(f"-D{define}")
    for inc in include_dirs:
        cmd += ["-I", inc]
    cmd += [source, "-o", out]
    result = Run(cmd)
    if result.returncode != 0:
        raise CookError(f"glslc -E failed for {source}:\n{result.stderr}")


def CookFile(source: Path, stage: str, glslc: Path, naga: Path, tint: Path | None,
             validate: bool, defines: list[str], includeDirs: list[Path],
             outWgsl: Path, scratch: Path) -> None:
    flattened = scratch / (source.stem + ".pp" + source.suffix)
    Preprocess(source, stage, glslc, defines, includeDirs, flattened)
    compileSource = scratch / (source.stem + ".rewritten" + source.suffix)
    compileSource.write_text(RewritePushConstants(flattened.read_text()))

    spv = scratch / (source.stem + ".spv")
    command = [glslc, "--target-env=vulkan1.1", f"-fshader-stage={stage}", "-O",
           compileSource, "-o", spv]
    result = Run(command)
    if result.returncode != 0:
        raise CookError(f"glslc failed for {source}:\n{result.stderr}")

    split = scratch / (source.stem + ".split.spv")
    spirvOptimizer = FindTool("spirv-opt", None)
    if spirvOptimizer is None:
        raise CookError("spirv-opt not found (Vulkan SDK) - required for the "
                        "combined-image-sampler split")
    result = Run([spirvOptimizer, "--split-combined-image-sampler", spv, "-o", split])
    if result.returncode != 0:
        raise CookError(f"spirv-opt split failed for {source}:\n{result.stderr}")

    patched, remap = RenumberSamplerBindings(HoistDescriptorTypes(split.read_bytes()))
    split.write_bytes(patched)
    if remap:
        (outWgsl.with_suffix(".remap.json")).write_text(
            __import__("json").dumps({"samplerBindingOffset": kSamplerBindingOffset,
                                      "samplers": remap}, indent=1))

    result = Run([naga, "--keep-coordinate-space", split, outWgsl])
    if result.returncode != 0:
        raise CookError(f"naga failed for {source}:\n{result.stderr or result.stdout}")

    if validate:
        if tint is None:
            raise CookError(
                "tint not found - run Tools/ShaderCook/toolchain/setup.py to fetch it. "
                "WGSL validation is required by default because unvalidated WGSL is exactly "
                "what browsers reject at runtime. Pass --no-validate to cook unvalidated "
                "WGSL at your own risk.")
        result = Run([tint, "--format", "wgsl", outWgsl])
        if result.returncode != 0:
            raise CookError(f"tint rejected {outWgsl}:\n{result.stderr or result.stdout}")


def SelfTest(glslc: Path, naga: Path, tint: Path | None, validate: bool) -> int:
    with tempfile.TemporaryDirectory() as tempdir:
        scratch = Path(tempdir)
        source = scratch / "selftest.comp"
        source.write_text(kSelfTestShader)
        out = scratch / "selftest.wgsl"
        CookFile(source, "compute", glslc, naga, tint, validate, [], [], out, scratch)
        wgsl = out.read_text()
        if "@compute" not in wgsl or "workgroup_size(64" not in wgsl:
            raise CookError("self-test WGSL is missing the compute entry point")
    print("shadercook self-test OK "
          f"(glslc={glslc}, naga={naga}, tint={tint if tint else 'ABSENT'}, "
          f"validated={'yes' if validate and tint else 'NO'})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sources", nargs="*", type=Path,
                        help="GLSL stage files (.vert/.frag/.comp)")
    parser.add_argument("--out-dir", type=Path, default=Path("."),
                        help="directory for the emitted .wgsl files")
    parser.add_argument("--define", "-D", action="append", default=[], dest="defines")
    parser.add_argument("--include", "-I", action="append", default=[], dest="includes",
                        type=Path)
    parser.add_argument("--no-validate", action="store_true",
                        help="skip the tint conformance gate (explicit risk opt-in)")
    parser.add_argument("--glslc", help="override glslc path")
    parser.add_argument("--naga", help="override naga path")
    parser.add_argument("--tint", help="override tint path")
    parser.add_argument("--self-test", action="store_true",
                        help="cook an embedded shader through the whole chain")
    args = parser.parse_args()

    glslc = FindTool("glslc", args.glslc)
    naga = FindTool("naga", args.naga)
    tint = FindTool("tint", args.tint)
    if glslc is None:
        print("shadercook: glslc not found (Vulkan SDK or --glslc)", file=sys.stderr)
        return 2
    if naga is None:
        print("shadercook: naga not found - run Tools/ShaderCook/toolchain/setup.py "
              "to fetch it (or pass --naga)", file=sys.stderr)
        return 2

    try:
        if args.self_test:
            return SelfTest(glslc, naga, tint, validate=not args.no_validate)

        if not args.sources:
            parser.error("no sources given (or use --self-test)")
        args.out_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as tempdir:
            for source in args.sources:
                stage = kStageByExtension.get(source.suffix)
                if stage is None:
                    raise CookError(f"cannot infer shader stage from '{source}' "
                                    f"(expected one of {sorted(kStageByExtension)})")
                out = args.out_dir / (source.stem + ".wgsl")
                CookFile(source, stage, glslc, naga, tint, not args.no_validate,
                         args.defines, args.includes, out, Path(tempdir))
                print(f"cooked {source} -> {out}")
    except CookError as error:
        print(f"shadercook: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
