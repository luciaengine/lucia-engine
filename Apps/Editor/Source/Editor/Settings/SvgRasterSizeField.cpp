#include "Editor/Settings/SvgRasterSizeField.h"

#include "Assets/SvgRasterizer.h"
#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SvgRasterSettings.h"
#include "Logger/Logger.h"

#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
// Offered sizes are 2^kMinExponent .. 2^kMaxExponent pixels on the long edge.
constexpr int kMinExponent = 4;
constexpr int kMaxExponent = 14;

std::vector<SettingsFieldDescriptor::DropdownField::Option> RasterSizeOptions()
{
    std::vector<SettingsFieldDescriptor::DropdownField::Option> options;
    options.reserve(static_cast<size_t>(kMaxExponent - kMinExponent + 1));
    for (int exponent = kMinExponent; exponent <= kMaxExponent; ++exponent)
    {
        const std::string pixels = std::to_string(1 << exponent);
        options.push_back({pixels, pixels + "x" + pixels});
    }
    return options;
}

// The stored size rounded up to the nearest offered option, so a hand-edited
// value still selects an entry.
std::string StoredRasterSize(SvgRasterSettingsScope scope)
{
    std::string loadError;
    const uint32_t pixels =
        SvgRasterSettings::Load(scope, EngineCore::GetInstance().GetWorkspaceRoot(), &loadError);
    if (!loadError.empty())
        Logger::Log::Warning("SvgRasterSettings: failed to load setting: {}", loadError);
    int exponent = kMinExponent;
    while (exponent < kMaxExponent && (1u << exponent) < pixels)
        ++exponent;
    return std::to_string(1 << exponent);
}

void SaveRasterSize(SvgRasterSettingsScope scope, const std::string& value)
{
    uint32 pixels = 0;
    if (!ParseSvgRasterSizeMeta(value, pixels))
        return;
    std::string err;
    if (!SvgRasterSettings::SaveAndApply(scope, EngineCore::GetInstance().GetWorkspaceRoot(), pixels, true, &err))
        Logger::Log::Warning("SvgRasterSettings: failed to save setting: {}", err);
}
} // namespace

SettingsFieldDescriptor MakeSvgRasterSizeField(SvgRasterSettingsScope scope)
{
    const bool editorUi = scope == SvgRasterSettingsScope::EditorUi;

    SettingsFieldDescriptor field;
    field.Label = editorUi ? "UI SVG Raster Size" : "Default Raster Size";
    field.Tooltip = editorUi
                        ? "Default raster bounds for SVGs shipped with the editor UI. Aspect ratio is preserved."
                        : "Default raster bounds for project SVG textures without a per-asset override. Aspect "
                          "ratio is preserved.";
    field.SearchKeywords = editorUi ? "ui svg raster resolution size icons"
                                    : "svg texture import raster resolution default size";
    SettingsFieldDescriptor::DropdownField dropdown;
    dropdown.OptionsProvider = RasterSizeOptions;
    dropdown.DefaultValue = std::to_string(SvgRasterSettings::kDefaultSize);
    dropdown.Get = [scope]() { return StoredRasterSize(scope); };
    dropdown.Set = [scope](const std::string& value) { SaveRasterSize(scope, value); };
    field.Control = std::move(dropdown);
    return field;
}

} // namespace GameEngine::Editor
