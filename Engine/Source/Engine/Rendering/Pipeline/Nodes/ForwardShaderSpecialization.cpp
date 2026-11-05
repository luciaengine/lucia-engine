#include "Engine/Rendering/Pipeline/Nodes/ForwardShaderSpecialization.h"

#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/SpecializationConstants.h"

namespace GameEngine::Engine::Renderer
{
namespace
{
constexpr uint32_t kLocalLightsConstant = 300;
constexpr uint32_t kLocalProbesConstant = 301;
constexpr uint32_t kHexTilingConstant = 302;
constexpr uint32_t kShadowFilterConstant = 303;
constexpr uint32_t kShadowDebugConstant = 304;
constexpr uint32_t kSecondaryLightsConstant = 305;
constexpr int32_t kRuntimeShadowFilter = -1;

bool MayUseHexTiling(const Material& material)
{
    const auto& spec = material.GetCompileSpec();
    if (spec.customVertexShader || !spec.vertexModifierPath.empty())
        return true;
    if (spec.surfaceShaderPath.empty() || spec.surfaceShaderPath == "Surfaces/standard_surface.glsl")
        return false;
    if (spec.surfaceShaderPath != "Surfaces/standard_pbr.glsl")
        return true;
    return !(material.GetFloat("hexTiling"_sid, 0.0f) < 0.5f);
}
}

bool ForwardShaderSpecialization::HasHexMaterials(RenderServices& services)
{
    const auto epoch = Material::GetGlobalContentEpoch();
    const auto& registry = services.Materials().Registry();
    const auto count = registry.GetMaterialCount();
    if (epoch != m_MaterialEpoch || count != m_MaterialCount)
    {
        m_HexMaterials = false;
        registry.ForEach([this](const auto&, const Material& material)
        {
            m_HexMaterials |= MayUseHexTiling(material);
        });
        m_MaterialEpoch = epoch;
        m_MaterialCount = count;
    }
    return m_HexMaterials;
}

std::shared_ptr<const Rendering::SpecializationConstants> ForwardShaderSpecialization::Resolve(
    RenderServices& services, const Rendering::ViewDesc& view)
{
    if (!services.GetDevice() ||
        !services.GetDevice()->GetCapabilities().prefersForwardShaderSpecialization)
        return {};

    int32_t localLights = 0;
    uint32_t directionalLights = 0;
    for (const auto& light : services.GetWorldLights(view.worldId))
    {
        if (light.type == Components::LightType::Directional)
            ++directionalLights;
        else
            localLights = 1;
    }
    int32_t localProbes = 1;
    if (const auto* ibl = services.GetFeature<ImageBasedLightingFeature>())
    {
        const auto world = ibl->IsLocalProbeCaptureView(view.id) ? 0 : view.worldId;
        localProbes = ibl->GetLocalProbeCount(world) != 0 ? 1 : 0;
    }
    int32_t shadowFilter = kRuntimeShadowFilter;
    int32_t shadowDebug = 1;
    if (auto* shadows = services.GetFeature<ShadowMapRenderFeature>())
    {
        if (shadows->ResolveEffectiveFilterQuality(services, view.id) == ShadowFilterQuality::Grid5x5)
            shadowFilter = static_cast<int32_t>(ShadowFilterQuality::Grid5x5);
        shadowDebug = shadows->GetDebugMode() != ShadowDebugMode::Off ? 1 : 0;
    }
    const std::array<int32_t, 6> state{
        localLights, localProbes, HasHexMaterials(services) ? 1 : 0, shadowFilter, shadowDebug,
        directionalLights > 1 ? 1 : 0};
    if (!m_Constants || m_State != state)
    {
        auto constants = std::make_shared<Rendering::SpecializationConstants>();
        // Vulkan specialization booleans occupy four bytes.
        constants->AddConstant(kLocalLightsConstant, static_cast<uint32_t>(state[0]));
        constants->AddConstant(kLocalProbesConstant, static_cast<uint32_t>(state[1]));
        constants->AddConstant(kHexTilingConstant, static_cast<uint32_t>(state[2]));
        constants->AddConstant(kShadowFilterConstant, state[3]);
        constants->AddConstant(kShadowDebugConstant, static_cast<uint32_t>(state[4]));
        constants->AddConstant(kSecondaryLightsConstant, static_cast<uint32_t>(state[5]));
        m_Constants = std::move(constants);
        m_State = state;
    }
    return m_Constants;
}
}
