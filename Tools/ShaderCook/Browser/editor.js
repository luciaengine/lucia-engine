let shaderTools;
Module.prepareShaderCompiler = (glslc, optimizer, naga) => {
    shaderTools = {
        createGlslc: options => startCompiler(GeGlslc, options, glslc),
        createOptimizer: options => startCompiler(GeSpirvOpt, options, optimizer),
        translator: new WebAssembly.Instance(new WebAssembly.Module(naga)).exports,
    };
};
Module.compileShaderStage = request => compileShaderStage(shaderTools, request);
