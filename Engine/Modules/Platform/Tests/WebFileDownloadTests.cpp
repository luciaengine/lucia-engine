#include "Platform/WebFileDownload.h"
#include <emscripten/em_js.h>
#include <cstdio>
#include <string>

EM_JS(void, InstallBrowserDouble, (), {
    globalThis.downloadTest = {clicked: 0, removed: 0, revoked: 0, fail: false};
    globalThis.Blob = class { constructor(parts) { downloadTest.bytes = Array.from(parts[0]); } };
    globalThis.URL = {
        createObjectURL: () => 'blob:test',
        revokeObjectURL: () => { downloadTest.revoked++; }
    };
    globalThis.document = {
        body: {appendChild: () => {}},
        createElement: () => ({
            click() {
                if (downloadTest.fail) throw new Error('intentional test failure');
                downloadTest.name = this.download;
                downloadTest.clicked++;
            },
            remove() { downloadTest.removed++; }
        })
    };
    globalThis.setTimeout = callback => { callback(); return 0; };
});
EM_JS(int, CheckBrowserRequest, (), {
    return downloadTest.name === 'example.po' && downloadTest.clicked === 1 &&
        downloadTest.removed === 1 && downloadTest.revoked === 1 &&
        JSON.stringify(downloadTest.bytes) === '[65,0,195,169]';
});
EM_JS(void, FailBrowserRequest, (), { downloadTest.fail = true; });
EM_JS(int, CheckFailureCleanup, (), {
    return downloadTest.clicked === 1 && downloadTest.removed === 2 && downloadTest.revoked === 2;
});
int main()
{
    InstallBrowserDouble();
    using GameEngine::Platform::DownloadFile;
    if (DownloadFile("", "x") || DownloadFile("..", "x") ||
        DownloadFile(std::string("bad\0name", 8), "x")) return 1;
    if (!DownloadFile("folder/example.po", std::string("A\0\xc3\xa9", 4))) return 2;
    if (!CheckBrowserRequest()) return 3;
    FailBrowserRequest();
    if (DownloadFile("failure.po", "x")) return 4;
    if (!CheckFailureCleanup()) return 5;
    std::puts("Web file download: names, binary bytes, browser request and failure cleanup passed.");
    return 0;
}
