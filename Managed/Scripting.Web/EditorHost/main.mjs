import { dotnet } from './_framework/dotnet.js';

const pageModule = globalThis.Module;
const { canvas, print, printErr } = pageModule;
try {
    // Reserve loaded workers for Mono, the engine job pool, asset IO, and OPFS at startup.
    const { Module, getAssemblyExports, getConfig, runMain } = await dotnet
        .withConfig({ pthreadPoolInitialSize: 16, pthreadPoolUnusedSize: 4 })
        .withModuleConfig({ canvas, print, printErr }).create();
    await runMain(getConfig().mainAssemblyName, []);
    const installEditorAssets = globalThis.geInstallEditorAssets;
    delete globalThis.geInstallEditorAssets;
    await installEditorAssets(Module);
    const exports = await getAssemblyExports('Lucia.Web.Editor.dll');
    const scripts = exports.Lucia.Web.Editor.BrowserPlayMode;
    const scriptSession = createScriptSession(Module, scripts);
    Module.webScriptSession = scriptSession;
    globalThis.Module = Module;
    await prepareStorage(Module);
    const status = await Module.ccall('GeStartEditor', 'number', [], [], { async: true });
    if (status !== 0)
        throw new Error('The native editor failed to start.');
    pageModule.onRuntimeInitialized();

    async function frame() {
        try {
            const keepRunning = await Module.ccall('GeTickEditor', 'boolean', [], [], { async: true });
            await scriptSession.runPending();
            if (keepRunning)
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

function createScriptSession(module, scripts) {
    let pending;
    let running = false;
    const reportScriptError = diagnostic =>
        module.ccall('GeReportBrowserScriptError', null, ['string'], [diagnostic]);

    // C++ copies requests into this slot while the native frame is on the
    // stack. All managed calls run after that frame has returned to JavaScript.
    return {
        start(world, sources) {
            pending = async () => {
                const diagnostic = await scripts.Start(world, sources);
                running = !diagnostic;
                if (diagnostic)
                    reportScriptError(diagnostic);
            };
        },
        tick(delta) {
            if (running && !pending)
                pending = () => scripts.Tick(delta);
        },
        requestStop() {
            if (!running && !pending)
                return true;
            pending = async () => {
                running = false;
                await scripts.Stop();
            };
            return false;
        },
        async runPending() {
            const operation = pending;
            pending = undefined;
            if (!operation)
                return;
            try {
                await operation();
            } catch (error) {
                reportScriptError(String(error));
                if (running) {
                    running = false;
                    try {
                        await scripts.Stop();
                    } catch (stopError) {
                        reportScriptError(String(stopError));
                    }
                }
            }
        },
    };
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
