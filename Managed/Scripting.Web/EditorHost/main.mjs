import { dotnet } from './_framework/dotnet.js';

const pageModule = globalThis.Module;
try {
    // Use the SDK's workers so Mono and native OPFS jobs share the same worker pool.
    const { Module, getAssemblyExports } = await dotnet
        .withConfig({ pthreadPoolInitialSize: 12, pthreadPoolUnusedSize: 4 })
        .withModuleConfig(pageModule).create();
    const exports = await getAssemblyExports('Lucia.Web.Editor.dll');
    const scripts = exports.Lucia.Web.Editor.BrowserPlayMode;
    Module.webScriptSession = {
        start: scripts.Start,
        tick: scripts.Tick,
        stop: scripts.Stop,
    };
    globalThis.Module = Module;
    await prepareStorage(Module);
    const status = await Module.ccall('GeStartEditor', 'number', [], [], { async: true });
    if (status !== 0)
        throw new Error('The native editor failed to start.');
    pageModule.onRuntimeInitialized();

    async function frame() {
        try {
            await Module.ccall('GeTickEditor', null, [], [], { async: true });
            scheduleFrame();
        } catch (error) {
            report(error);
        }
    }

    function scheduleFrame() {
        if (document.hidden)
            setTimeout(frame, 33);
        else
            requestAnimationFrame(frame);
    }
    scheduleFrame();
} catch (error) {
    report(error);
}

async function prepareStorage(module) {
    module.ccall('GePrepareEditorStorage', null, [], []);
    const deadline = performance.now() + 30_000;
    while (!module.ccall('GeEditorStoragePrepared', 'boolean', [], [])) {
        if (performance.now() >= deadline)
            throw new Error('Project storage did not start.');
        await new Promise(resolve => setTimeout(resolve, 10));
    }
}

function report(error) {
    console.error(error);
    const status = document.getElementById('msg');
    if (status)
        status.textContent = String(error);
}
