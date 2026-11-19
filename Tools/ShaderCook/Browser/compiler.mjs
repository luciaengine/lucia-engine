import {normalizeSpirv} from './spirv.mjs';

const SourceRoot = '/shader-sources';
const SourceSpirv = '/source.spv';
const SplitSpirv = '/split.spv';
const PreprocessedSource = '/preprocessed.glsl';
const RewrittenSource = '/rewritten.glsl';
const ShaderStages = new Set(['vert', 'frag', 'comp']);

function sourcePath(sourceName) {
    const segments = sourceName.split('/');
    if (sourceName.includes('\\') || segments.some(segment => !segment || segment === '.' || segment === '..')) {
        throw new Error(`Invalid shader source path: ${sourceName}`);
    }
    return `${SourceRoot}/${sourceName}`;
}

function installSources(fileSystem, sources) {
    const directories = new Set([SourceRoot]);
    fileSystem.mkdir(SourceRoot);
    for (const [name, source] of Object.entries(sources)) {
        const path = sourcePath(name);
        const parent = path.slice(0, path.lastIndexOf('/'));
        if (!directories.has(parent)) {
            fileSystem.mkdirTree(parent);
            directories.add(parent);
        }
        fileSystem.writeFile(path, source);
    }
}

function startCompiler(factory, options, wasmBinary) {
    let initialized = false;
    const instance = {
        ...options, wasmBinary,
        onRuntimeInitialized() { initialized = true; },
    };
    // The tool has no asynchronous imports or preload dependencies. With
    // synchronous Wasm loading it initializes the supplied module on this stack.
    factory(instance).catch(error => console.error(error));
    if (!initialized) {
        throw new Error('The shader compiler could not initialize synchronously.');
    }
    return instance;
}

function runCompiler(program, argumentsList, label, diagnostics) {
    diagnostics.length = 0;
    const status = program.callMain(argumentsList);
    if (status !== 0) {
        throw new Error(`${label} failed with exit code ${status}\n${diagnostics.join('\n')}`);
    }
}

function translateSpirv(translator, words) {
    const inputPointer = translator.input_buffer(words.length);
    new Uint32Array(translator.memory.buffer, inputPointer, words.length).set(words);
    const status = translator.translate_spirv();
    const output = new Uint8Array(translator.memory.buffer, translator.output_buffer(), translator.output_size());
    const text = new TextDecoder().decode(output);
    if (status !== 0) {
        throw new Error(text);
    }
    return text;
}

function compileShaderStage(tools, {sourceName, stage, sources, includeDirectories, defines, reflectionTarget}) {
    if (!ShaderStages.has(stage)) {
        throw new Error(`Unsupported browser shader stage: ${stage}`);
    }
    if (!Object.hasOwn(sources, sourceName)) {
        throw new Error(`Missing shader source: ${sourceName}`);
    }

    const diagnostics = [];
    const printErr = message => diagnostics.push(message);
    const glslc = tools.createGlslc({printErr});
    const optimizer = tools.createOptimizer({printErr});
    installSources(glslc.FS, sources);

    const entryFile = sourcePath(sourceName);
    const stageFlag = '-fshader-stage=' + stage;
    const includeFlags = includeDirectories.map(path => '-I' + (path === '.' ? SourceRoot : sourcePath(path)));
    const defineFlags = defines.map(define => '-D' + define);
    let words;
    if (reflectionTarget) {
        runCompiler(glslc, [stageFlag, '--target-env=' + reflectionTarget, '-O0',
            ...defineFlags, ...includeFlags, entryFile, '-o', SourceSpirv],
            `${sourceName} SPIR-V reflection input`, diagnostics);
        const spirv = glslc.FS.readFile(SourceSpirv);
        words = Array.from(new Uint32Array(spirv.buffer, spirv.byteOffset, spirv.length / 4));
    }
    runCompiler(glslc, ['-E', stageFlag, '-DGE_WGSL_COOK=1',
        ...defineFlags, ...includeFlags, entryFile, '-o', PreprocessedSource],
    `${sourceName} preprocessing`, diagnostics);

    // Group 3 is reserved for the WebGPU uniform buffer that carries push constants.
    const preprocessed = glslc.FS.readFile(PreprocessedSource, {encoding: 'utf8'});
    const rewritten = preprocessed.replace(/layout\s*\(\s*push_constant\s*\)/g, 'layout(set = 3, binding = 0)');
    glslc.FS.writeFile(RewrittenSource, rewritten);
    runCompiler(glslc, ['--target-env=vulkan1.1', stageFlag, '-O', RewrittenSource, '-o', SourceSpirv],
        `${sourceName} GLSL compilation`, diagnostics);

    optimizer.FS.writeFile(SourceSpirv, glslc.FS.readFile(SourceSpirv));
    runCompiler(optimizer, ['--split-combined-image-sampler', SourceSpirv, '-o', SplitSpirv],
        `${sourceName} sampler split`, diagnostics);
    const spirv = normalizeSpirv(optimizer.FS.readFile(SplitSpirv));
    return {wgsl: translateSpirv(tools.translator, spirv), words};
}

/**
 * Loads the browser compiler assets. The returned function compiles one GLSL
 * stage and resolves to WGSL. Paths in a request are relative to its sources.
 */
export async function createBrowserShaderCompiler(toolDirectory) {
    const [glslc, optimizer, translator, glslcWasm, optimizerWasm] = await Promise.all([
        import(new URL('glslc.mjs', toolDirectory)),
        import(new URL('spirv-opt.mjs', toolDirectory)),
        WebAssembly.instantiateStreaming(fetch(new URL('naga-browser.wasm', toolDirectory))),
        fetch(new URL('glslc.wasm', toolDirectory)).then(response => response.arrayBuffer()),
        fetch(new URL('spirv-opt.wasm', toolDirectory)).then(response => response.arrayBuffer()),
    ]);
    const tools = {
        createGlslc: options => startCompiler(glslc.default, options, new Uint8Array(glslcWasm)),
        createOptimizer: options => startCompiler(optimizer.default, options, new Uint8Array(optimizerWasm)),
        translator: translator.instance.exports,
    };
    return request => compileShaderStage(tools, request).wgsl;
}
