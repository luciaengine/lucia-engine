#include "Platform/Shell.h"
#include <emscripten/em_js.h>
#include <fstream>
#include <iterator>
#include <string>
#include <cstdio>

EM_JS(void, InstallPickerDouble, (), {
    globalThis.pickerTest = {cancel: false, multiple: false};
    globalThis.showOpenFilePicker = async options => {
        pickerTest.multiple = options.multiple;
        if (pickerTest.cancel) throw new DOMException('Cancelled', 'AbortError');
        return [{getFile: async () => ({
            name: 'translation.po',
            arrayBuffer: async () => new Uint8Array([65, 0, 195, 169]).buffer
        })}];
    };
});
EM_JS(int, WasMultiple, (), { return pickerTest.multiple ? 1 : 0; });
EM_JS(void, CancelPicker, (), { pickerTest.cancel = true; });

int main()
{
    InstallPickerDouble();
    namespace Platform = GameEngine::Platform;
    const auto path = Platform::SelectFile({}, "PO translation", "*.po");
    if (path.empty() || WasMultiple() || path.filename() != "translation.po") return 1;
    std::ifstream input(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    input.close();
    if (bytes != std::string("A\0\xc3\xa9", 4)) return 2;
    const auto staging = path.parent_path();
    Platform::ReleaseTransientFiles({path});
    if (std::filesystem::exists(staging)) return 3;
    const auto paths = Platform::SelectFiles({}, "PO translation", "*.po");
    if (paths.size() != 1 || !WasMultiple()) return 4;
    Platform::ReleaseTransientFiles(paths);
    CancelPicker();
    if (!Platform::SelectFile({}, "PO translation", "*.po").empty()) return 5;
    std::puts("Web file picker: single/multiple selection, binary bytes, cleanup and cancellation passed.");
    return 0;
}
