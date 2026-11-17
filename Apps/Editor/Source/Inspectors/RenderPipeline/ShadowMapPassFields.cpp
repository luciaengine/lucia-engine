#include "Inspectors/RenderPipeline/ShadowMapPassFields.h"

#include "Engine/Rendering/Pipeline/Nodes/ShadowMapNode.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Toggle.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
using Engine::Renderer::Pipeline::Nodes::ShadowMapNode;

// The cascades are D32_FLOAT depth layers.
constexpr unsigned long long kCascadeBytesPerTexel = 4ull;
// Saved floats keep 4 decimals, so 0.2 saves as 0.2 and not as its float expansion.
constexpr double kSavedFloatScale = 10000.0;
constexpr float kMinMaxShadowDistance = 0.1f;

// The row tooltip, with the memory of one cascade at each supported size.
std::string CascadeResolutionTooltip()
{
    std::string text = "Size in texels of each directional shadow cascade. Higher values give sharper shadow "
                       "edges. Memory of one cascade:";
    for (uint32_t resolution = ShadowMapNode::kMinCascadeResolution;
         resolution <= ShadowMapNode::kMaxCascadeResolution; resolution *= 2u)
    {
        const unsigned long long bytes =
            static_cast<unsigned long long>(resolution) * resolution * kCascadeBytesPerTexel;
        text += (resolution == ShadowMapNode::kMinCascadeResolution ? " " : ", ") + std::to_string(resolution) +
                ": " + FormatMebibytes(bytes);
    }
    return text + ".";
}

std::vector<Dropdown::Option> MakeCascadeResolutionOptions(uint32_t current, int& selectedIndex)
{
    std::vector<Dropdown::Option> options;
    selectedIndex = -1;
    if (!ShadowMapNode::IsSupportedCascadeResolution(current))
    {
        options.push_back({std::to_string(current), std::to_string(current) + " (invalid)"});
        selectedIndex = 0;
    }
    for (uint32_t resolution = ShadowMapNode::kMinCascadeResolution;
         resolution <= ShadowMapNode::kMaxCascadeResolution; resolution *= 2u)
    {
        if (resolution == current)
            selectedIndex = static_cast<int>(options.size());
        options.push_back({std::to_string(resolution), std::to_string(resolution)});
    }
    return options;
}

std::vector<Dropdown::Option> MakeCascadeCountOptions(std::optional<uint64_t> current, int& selectedIndex)
{
    std::vector<Dropdown::Option> options;
    selectedIndex = -1;
    if (!current || !ShadowMapNode::IsSupportedCascadeCount(*current))
    {
        const std::string value = current ? std::to_string(*current) : std::string{};
        options.push_back({value, current ? value + " (invalid)" : "(invalid)"});
        selectedIndex = 0;
    }
    for (uint32_t count = 1u; count <= ShadowMapNode::kMaxCascadeCount; ++count)
    {
        if (count == current)
            selectedIndex = static_cast<int>(options.size());
        options.push_back({std::to_string(count), std::to_string(count)});
    }
    return options;
}

double SavedFloat(float value)
{
    return std::round(static_cast<double>(value) * kSavedFloatScale) / kSavedFloatScale;
}

uint32_t ReadUnsigned(const nlohmann::json& pass, const char* key, uint32_t fallback)
{
    const auto it = pass.find(key);
    return it != pass.end() && it->is_number_unsigned() ? it->get<uint32_t>() : fallback;
}

float ReadFloat(const nlohmann::json& pass, const char* key, float fallback)
{
    const auto it = pass.find(key);
    return it != pass.end() && it->is_number() ? it->get<float>() : fallback;
}

bool ReadBool(const nlohmann::json& pass, const char* key, bool fallback)
{
    const auto it = pass.find(key);
    return it != pass.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

void AddCascadeCountRow(UIElement& section, const nlohmann::json& pass, const std::string& idPrefix,
                        const PipelinePassFieldCommit& commit)
{
    int selectedIndex = 0;
    const auto configured = pass.find("cascades");
    std::optional<uint64_t> current = ShadowMapNode::kDefaultCascadeCount;
    if (configured != pass.end())
    {
        if (configured->is_number_unsigned())
            current = configured->get<uint64_t>();
        else
            current.reset();
    }
    const std::vector<Dropdown::Option> options = MakeCascadeCountOptions(current, selectedIndex);
    Dropdown* dropdown = InspectorUI::AddDropdownRow(
        &section, "Cascades", options, selectedIndex,
        "Number of directional shadow cascades. More cascades keep shadows sharp farther from the camera. "
        "Each cascade adds one depth pass and one layer of Resolution. The Cascade Count of the light can "
        "lower this number.");
    dropdown->SetId(idPrefix + "-cascades");
    dropdown->SetOnValueChanged([commit](const std::string& value)
                                {
                                    if (value.empty())
                                        return;
                                    const uint64_t count = std::stoull(value);
                                    if (ShadowMapNode::IsSupportedCascadeCount(count))
                                        commit("cascades", static_cast<uint32_t>(count), "Shadow Cascades");
                                });
}

void AddResolutionRow(UIElement& section, const nlohmann::json& pass, const std::string& idPrefix,
                      const PipelinePassFieldCommit& commit)
{
    int selectedIndex = 0;
    const std::vector<Dropdown::Option> options = MakeCascadeResolutionOptions(
        ReadUnsigned(pass, "resolution", ShadowMapNode::kDefaultCascadeResolution), selectedIndex);
    const std::string tooltip = CascadeResolutionTooltip();
    Dropdown* dropdown =
        InspectorUI::AddDropdownRow(&section, "Resolution", options, selectedIndex, tooltip.c_str());
    dropdown->SetId(idPrefix + "-resolution");
    dropdown->SetOnValueChanged([commit](const std::string& value)
                                { commit("resolution", static_cast<uint32_t>(std::stoul(value)), "Shadow Resolution"); });
}

void AddSplitLambdaRow(UIElement& section, const nlohmann::json& pass, const std::string& idPrefix,
                       const PipelinePassFieldCommit& commit)
{
    FloatField* field = InspectorDrag::AddFloatRowWithDrag(
        &section, "Split", ReadFloat(pass, "splitLambda", ShadowMapNode::kDefaultSplitLambda),
        [](float) {}, [commit](float value) { commit("splitLambda", SavedFloat(value), "Split Lambda"); },
        ShadowMapNode::kDefaultSplitLambda,
        "Split lambda: how the cascades divide the shadow distance. 0 gives cascades of equal depth. 1 "
        "gives more resolution near the camera.",
        0.0f, 1.0f);
    field->SetId(idPrefix + "-splitLambda");
}

void AddDistanceRows(UIElement& section, const nlohmann::json& pass, const std::string& idPrefix,
                     const PipelinePassFieldCommit& commit)
{
    const bool fitToScene = ReadBool(pass, "fitShadowDistanceToScene", false);
    Toggle* toggle = InspectorDrag::AddToggleRow(
        &section, "Fit To Scene", fitToScene,
        [commit](bool value) { commit("fitShadowDistanceToScene", value, "Fit Shadow Distance To Scene"); },
        "Fits the shadow distance to the meshes of the scene in each view. When on, Distance is not used.");
    toggle->SetId(idPrefix + "-fitShadowDistanceToScene");
    if (fitToScene)
        return;

    FloatField* field = InspectorDrag::AddFloatRowWithDrag(
        &section, "Distance",
        ReadFloat(pass, "maxShadowDistance", ShadowMapNode::kDefaultMaxShadowDistance), [](float) {},
        [commit](float value) { commit("maxShadowDistance", SavedFloat(value), "Max Shadow Distance"); },
        ShadowMapNode::kDefaultMaxShadowDistance,
        "Max shadow distance: the distance in meters from the camera at which directional shadows end.", kMinMaxShadowDistance,
        std::numeric_limits<float>::infinity());
    field->SetId(idPrefix + "-maxShadowDistance");
}
} // namespace

void BuildShadowMapPassFields(UIElement& passSection, const nlohmann::json& pass, const std::string& idPrefix,
                              PipelinePassFieldCommit commit)
{
    AddCascadeCountRow(passSection, pass, idPrefix, commit);
    AddResolutionRow(passSection, pass, idPrefix, commit);
    AddSplitLambdaRow(passSection, pass, idPrefix, commit);
    AddDistanceRows(passSection, pass, idPrefix, commit);
}

} // namespace GameEngine
