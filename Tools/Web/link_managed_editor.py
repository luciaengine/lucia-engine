#!/usr/bin/env python3
"""Link the threaded WebEditor objects into the .NET browser host."""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import xml.etree.ElementTree as ET


def editor_inputs(arguments):
    """Keep editor Emscripten settings; let the .NET SDK own its entry point."""
    options = []
    objects = []
    output = None
    shell = None
    args = iter(arguments)
    for arg in args:
        if arg == "-o":
            output = Path(next(args)).resolve()
        elif arg == "--shell-file":
            shell = Path(next(args)).resolve()
        elif arg == "-s":
            setting = next(args)
            if setting.split("=")[0] not in {"INITIAL_MEMORY", "EXPORTED_RUNTIME_METHODS", "ENVIRONMENT", "PTHREAD_POOL_SIZE", "MALLOC"}:
                options.append("-s" + setting)
        elif arg in {"--pre-js", "--extern-pre-js", "--post-js", "--js-library", "--preload-file", "--embed-file", "--exclude-file"}:
            value = next(args)
            options.extend([arg, value])
        elif arg.endswith((".o", ".a")):
            objects.append(Path(arg).resolve())
        elif arg not in {"-O3", "-O2", "-O1", "-O0", "-DNDEBUG", "-fuse-ld=mold", "-pthread"}:
            options.append(arg)
    if output is None or shell is None or not objects:
        raise ValueError("WebEditor link command is missing its output, shell, or native inputs")
    return output, shell, objects, options


def write_file(path, content):
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


def write_link_inputs(build, objects, options):
    flags = build / "native-link.rsp"
    write_file(flags, "\n".join(map(shlex.quote, options)) + "\n")
    project = ET.Element("Project")
    group = ET.SubElement(project, "PropertyGroup")
    ET.SubElement(group, "EmccExtraLDFlags").text = "@" + str(flags)
    items = ET.SubElement(project, "ItemGroup")
    # Ninja invokes this launcher only after a native, managed, or preload input changes.
    # The SDK cannot see assets named inside emcc's nested response file.
    stamp = build / "native-inputs.stamp"
    stamp.touch()
    ET.SubElement(items, "_WasmLinkDependencies", {"Include": str(stamp)})
    for path in dict.fromkeys(objects):
        # Mono's static P/Invoke table maps this archive's basename to the managed library.
        attributes = {"Include": str(path), "ScanForPInvokes": "false"}
        if path.name == "GameEngine.ECS.ABI.a":
            del attributes["ScanForPInvokes"]
        ET.SubElement(items, "NativeFileReference", attributes)
    refs = build / "NativeReferences.targets"
    write_file(refs, ET.tostring(project, encoding="unicode"))
    return refs


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError("The pinned .NET browser glue changed: " + before.splitlines()[0])
    return source.replace(before, after)



def use_native_opfs_callbacks(source):
    # A threaded WasmFS proxy already waits for emscripten_proxy_finish. Let its
    # browser Promise complete without suspending the native worker's stack.
    async_import = (
        r"(var __wasmfs_opfs_\w+ = function\(ctx[^\n]*\n(?:(?!\n};).)*?)"
        r"return Asyncify\.handleAsync\(innerFunc\);"
    )
    source, async_count = re.subn(async_import, r"""\1if (ENVIRONMENT_IS_PTHREAD && ctx) {
    innerFunc().catch(error => console.error("Project storage failed", error));
    return;
  }
  return Asyncify.handleAsync(innerFunc);""", source, flags=re.DOTALL)
    # Read and write on a SyncAccessHandle return their byte counts immediately.
    sync_import = (
        r"(var __wasmfs_opfs_(?:read|write)_access = function\([^\n]+\n(?:(?!\n};).)*?)"
        r"return Asyncify\.handleAsync\(innerFunc\);"
    )
    source, sync_count = re.subn(
        sync_import, r"\1return ENVIRONMENT_IS_PTHREAD ? innerFunc() : Asyncify.handleAsync(innerFunc);",
        source, flags=re.DOTALL,
    )
    if async_count == 0 or sync_count != 2:
        raise ValueError("The pinned .NET project storage bindings changed")
    return source


def split_editor_data(source, archive, framework):
    """Keep only picker assets in the preload that blocks editor startup."""
    package_call = re.search(r"^  loadPackage\(", source, re.MULTILINE)
    if package_call is None:
        raise ValueError("The pinned .NET editor preload manifest is missing")
    start = package_call.end()
    metadata, length = json.JSONDecoder().raw_decode(source[start:])
    end = start + length
    if not source.startswith(");", end) or metadata["remote_package_size"] != archive.stat().st_size:
        raise ValueError("The .NET preload manifest does not match the data file")

    startup_files = []
    shader_files = []
    startup_name = framework / "dotnet.native.data"
    shader_name = framework / "dotnet.native.shaders.data"
    with archive.open("rb") as data, startup_name.open("wb") as startup, shader_name.open("wb") as shaders:
        for entry in metadata["files"]:
            begin = int(entry["start"])
            finish = int(entry["end"])
            if (begin != entry["start"] or finish != entry["end"] or
                    begin != data.tell() or not begin <= finish <= metadata["remote_package_size"]):
                raise ValueError("Invalid .NET preload entry: " + entry["filename"])
            deferred = entry["filename"].startswith("/.Cache/Shaders/")
            output, entries = (shaders, shader_files) if deferred else (startup, startup_files)
            output_start = output.tell()
            output.write(data.read(finish - begin))
            entries.append({**entry, "start": output_start, "end": output.tell()})
        if data.tell() != metadata["remote_package_size"] or not startup_files or not shader_files:
            raise ValueError("The .NET editor preload is incomplete")
        startup_metadata = {"files": startup_files, "remote_package_size": startup.tell()}
        shader_metadata = {"files": shader_files, "remote_package_size": shaders.tell()}

    shader_call = f'\n  loadPackage({json.dumps(shader_metadata)}, "{shader_name.name}");'
    return source[:start] + json.dumps(startup_metadata) + source[end:end + 2] + shader_call + source[end + 2:]


def patch_glue(framework, archive):
    loader = framework / "dotnet.js"
    boot = loader.read_text()
    native, = (path for path in framework.glob("dotnet.native.*.js") if path.name in boot)
    source = native.read_text()
    patched = split_editor_data(source, archive, framework)
    # Leave Mono exports as actual Wasm functions because Mono puts them into its table.
    patched = replace_once(
        patched,
        'for (let [x, original] of Object.entries(exports)) {\n'
        '      if (typeof original == "function") {\n        var wrapper = Asyncify.instrumentFunction(original);',
        'for (let [x, original] of Object.entries(exports)) {\n'
        '      if (x === "GeStartEditor" || x === "GeTickEditor" || '
        'x === "_emscripten_check_mailbox" || x === "emscripten_proxy_execute_queue") {\n'
        '        var wrapper = Asyncify.instrumentFunction(original);',
    )
    # The SDK replaces Module fields during runtime setup. Resolve against the native module
    # URL and retain the download locally until Emscripten installs it into WasmFS.
    start = patched.index("    async function fetchRemotePackage(")
    end = patched.index("    var fetchPromise;", start)
    patched = patched[:start] + """    async function fetchRemotePackage(packageName) {
      const response = await globalThis.fetch(new URL(packageName, import.meta.url));
      if (!response.ok) throw new Error(`Editor data request failed: ${response.status}`);
      return await response.arrayBuffer();
    }
""" + patched[end:]
    # Both packs use the SDK's WasmFS installer. Start the deferred fetch only
    # when the editor requests a project, and retry a failed download on demand.
    patched = replace_once(patched, "async function loadPackage(metadata) {",
                           "async function loadPackage(metadata, deferredPackage) {")
    patched = replace_once(patched, 'var REMOTE_PACKAGE_BASE = "dotnet.native.data";',
                           'var REMOTE_PACKAGE_BASE = deferredPackage ?? "dotnet.native.data";')
    patched = replace_once(patched, "if (!fetched) {\n      // Note that",
                           "if (!fetched && !deferredPackage) {\n      // Note that")
    # Fetch outside the frame, then publish the cache between native calls.
    patched = replace_once(patched, "async function runWithFS(Module) {",
                           "async function prepareWithFS(Module) {")
    first_directory = patched.index('      Module["FS_createPath"](')
    process_signature = "      async function processPackageData(arrayBuffer) {\n"
    process = patched.index(process_signature, first_directory)
    directories = patched[first_directory:process]
    directories = re.sub(
        r'(?m)^      (Module\["FS_createPath"\]\("/\.Cache/Shaders.*)$',
        r'      if (deferredPackage) \1',
        directories,
    )
    patched = (
        patched[:first_directory]
        + "      function processPackageData(arrayBuffer) {\n"
        + "".join("  " + line for line in directories.splitlines(keepends=True))
        + patched[process + len(process_signature):]
    )
    patched = replace_once(patched, """      if (!fetched) {
        fetched = await fetchPromise;
      }
      await processPackageData(fetched);""", """      const bytes = fetched ?? await (fetchPromise ?? fetchRemotePackage(REMOTE_PACKAGE_NAME));
      fetched = undefined;
      fetchPromise = undefined;
      if (bytes.byteLength !== REMOTE_PACKAGE_SIZE) {
        throw new Error("Editor data size does not match this build.");
      }
      return () => processPackageData(bytes);""")
    # The picker can compile a shader before opening a project. Preserve that
    # cache entry when the cooked archive contains the same path.
    patched = replace_once(
        patched,
        '          Module["FS_createDataFile"](name, null, data, true, true, true);',
        """          try {
            Module["FS_createDataFile"](name, null, data, true, true, true);
          } catch (error) {
            const alreadyExists = 20; // WasmFS EEXIST
            if (!deferredPackage || error.errno !== alreadyExists) throw error;
          }""",
    )
    # Mono installs preRun itself. Install only with the final runtime Module.
    start = patched.index("    // Detect whether the module JS file has already been loaded.")
    end = patched.index("\n  }\n  loadPackage", start)
    patched = patched[:start] + """    if (deferredPackage)
      globalThis.gePrepareEditorShaders = activeModule => prepareWithFS(activeModule);
    else
      globalThis.geInstallEditorAssets = async activeModule => (await prepareWithFS(activeModule))();""" + patched[end:]
    patched, count = re.subn(
        r'(?m)^\s*Module\["(?:add|remove)RunDependency"\]\("datafile_[^\n]+\n', "\n", patched
    )
    if count != 2:
        raise ValueError("The pinned .NET editor data loader changed")
    # Emscripten's worker URL is emitted before the SDK fingerprints this module.
    patched = replace_once(patched, 'new URL("dotnet.native.js", import.meta.url)', 'new URL(import.meta.url)')
    # Asyncify has only one continuation per thread. Defer mailbox entry until
    # the native editor's suspended frame or startup call returns.
    patched = replace_once(patched, "var checkMailbox = () => {", """var checkMailbox = () => {
  if (Asyncify.state !== 0 || Asyncify.currData || Asyncify.exportCallStack.length) {
    setTimeout(checkMailbox, 0);
    return;
  }""")
    patched = replace_once(patched, "      _emscripten_proxy_execute_queue(queue);", """
      if (Asyncify.state === 0 && !Asyncify.currData && !Asyncify.exportCallStack.length)
        _emscripten_proxy_execute_queue(queue);""".lstrip("\n"))
    patched = use_native_opfs_callbacks(patched)
    native.write_text(patched)

    # Some SDK versions attach a digest to the native JS resource.
    def digest(value):
        return base64.b64encode(hashlib.sha256(value.encode()).digest()).decode()

    boot = boot.replace(digest(source), digest(patched))

    # The SDK sets info.isLoaded after a worker finishes loading. Its pool
    # selector reads worker.loaded and can strand native jobs on a new worker.
    runtime, = (path for path in framework.glob("dotnet.runtime.*.js") if path.name in boot)
    source = runtime.read_text()
    patched = replace_once(
        source,
        "if(n.loaded)return e.unusedWorkers.splice(t,1),n",
        "if(n.info.isLoaded)return e.unusedWorkers.splice(t,1),n",
    )
    runtime.write_text(patched)
    write_file(loader, boot.replace(digest(source), digest(patched)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keep-symbols", action="store_true")
    parser.add_argument("--dotnet", required=True)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--shaderpkg-dir", required=True, type=Path)
    parser.add_argument("--material-cache", required=True, type=Path)
    parser.add_argument("link", nargs=argparse.REMAINDER)
    config = parser.parse_args()
    if len(config.link) < 2 or config.link[0] != "--":
        parser.error("expected '-- em++ ...' after launcher options")

    arguments = []
    for arg in config.link[2:]:
        if arg.startswith("@"):
            arguments.extend(shlex.split(Path(arg[1:]).read_text()))
        else:
            arguments.append(arg)
    output, shell, objects, options = editor_inputs(arguments)
    build = Path.cwd() / "WebEditorManaged"
    build.mkdir(exist_ok=True)
    project = config.project.resolve()
    from builtin_shader_cook import FindShaderReflect, StripShaderPkgSpirv

    # The web backend only needs the WGSL of a cooked material stage.
    reflector = FindShaderReflect(config.shaderpkg_dir)
    StripShaderPkgSpirv(reflector, sorted(config.material_cache.rglob("*.shaderpkg")))
    references = write_link_inputs(build, objects, options)
    publish = build / "publish"
    subprocess.run(
        [
            config.dotnet, "publish", str(project), "-c", "Release", "--output", str(publish),
            "-p:LuciaEditorNativeReferences=" + str(references),
            "-p:ArtifactsPath=" + str(build / "artifacts"),
            "-p:WasmCachePath=" + str((build / "emscripten-cache").resolve()),
            "-p:WasmRunWasmOpt=" + ("false" if config.keep_symbols else "true"),
        ],
        check=True,
    )

    stage_editor(build, output, shell, project)


def stage_editor(build, output, shell, project):
    # Leave the SDK publish output intact. Incremental publish can reuse its
    # fingerprinted loader and runtime without copying them again.
    framework = build / "publish" / "wwwroot" / "_framework"
    staged_framework = output.parent / "_framework"
    shutil.copytree(framework, staged_framework, dirs_exist_ok=True)
    native_packages = sorted((build / "artifacts" / "obj").glob("**/for-publish/dotnet.native.data"))
    if len(native_packages) != 1:
        raise ValueError("Expected one .NET native editor data package")
    patch_glue(staged_framework, native_packages[0])
    shutil.copy2(project.with_name("main.mjs"), output.with_name("WebEditorManaged.mjs"))
    html = replace_once(shell.read_text(), "{{{ SCRIPT }}}",
                        '<script type="module" src="WebEditorManaged.mjs"></script>')
    output.write_text(html)


if __name__ == "__main__":
    main()
