"""Stage standalone ESM compilers and the editor's external JavaScript."""
from pathlib import Path
import re
import sys

root, tools, out = map(Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
bundle = []
for name, factory in (("glslc", "GeGlslc"), ("spirv-opt", "GeSpirvOpt")):
    script = (tools / (name + ".js")).read_text()
    (out / (name + ".mjs")).write_text(script + f"\nexport default {factory};\n")
    bundle.append(script)
for name in ("spirv.mjs", "compiler.mjs"):
    script = (root / name).read_text()
    bundle.append(re.sub(r"^import .*\n|^export ", "", script, flags=re.MULTILINE))
bundle.append((root / "editor.js").read_text())
# Emscripten's JS optimizer scans even nested functions by name. Keep the
# independently linked tools outside the editor module's optimizer.
(out / "editor-compiler.js").write_text("function installGeEditorShaderCompiler(Module) {\n" +
                                        "\n".join(bundle) + "\n}\n")
