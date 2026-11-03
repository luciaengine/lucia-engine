#pragma once

#include <cstdint>

namespace GameEngine::Editor
{
struct SettingsFieldDescriptor;
enum class SvgRasterSettingsScope : uint8_t;

// The SVG raster-size dropdown (power-of-two bounds, aspect ratio kept) for
// one SvgRasterSettings scope. The UI SVG size is a personal appearance
// setting; the texture-import size is a project-wide authoring rule.
SettingsFieldDescriptor MakeSvgRasterSizeField(SvgRasterSettingsScope scope);

} // namespace GameEngine::Editor
