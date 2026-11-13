#pragma once

#include "EditorApplication/EditorApplication.h"

#include <cstdint>
#include <vector>

namespace GameEngine::Rendering
{
enum class TextureFormat : uint32_t;
}

namespace GameEngine
{
class SceneViewController;
}

namespace GameEngine::Editor
{

// The deband threshold the screen's encode pass declared for the target window. The render
// callback copies it after that declaration, so a later window cannot change it before the
// conversion runs.
struct CaptureEncodeFilterState
{
    float DebandThresholdLsb = 0.0f;
};

// Converts readback bytes to RGBA8: depth to grayscale, float tone mapped. srgbEncode applies
// the linear-to-sRGB curve. dither and deband act only at a float-to-8-bit quantize, as the
// screen's encode pass does, never on an 8-bit source. A null filterState uses the process value.
std::vector<uint8_t> ConvertToRGBA8(const void* raw, uint32_t width, uint32_t height,
                                    Rendering::TextureFormat fmt, float rangeMin, float rangeMax,
                                    bool srgbEncode, bool dither, bool deband,
                                    const CaptureEncodeFilterState* filterState = nullptr);

// The scene view controller that renders the visible viewport of `window`. A maximized
// pane of the quad layout has its own controller, so a capture of `window.scene` would
// read a suspended view. Falls back to `window.scene` when no Scene View panel is mounted.
SceneViewController* ResolveActiveSceneController(EditorApplication::EditorWindowContext& window);

} // namespace GameEngine::Editor
