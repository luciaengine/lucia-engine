#include "Assets/ModelAsset.h"

#include "Assets/AssetManager.h"
#include "Assets/BoneLOD/BoneLODAssetSettings.h"
#include "Assets/BoneLOD/ModelBoneLOD.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Logger/Logger.h"

#include <algorithm>

namespace GameEngine
{

void ModelAsset::BuildBoneLOD()
{
    m_BoneLOD.reset();
    const auto* skeleton = Engine::Renderer::SkeletonStore::Instance().Get(m_SkeletonId);
    if (!skeleton)
        return;
    const bool haveSkin = std::any_of(m_Meshes.begin(), m_Meshes.end(),
        [](const Mesh& mesh) { return mesh.Skinned; });
    if (!haveSkin)
        return;
    BoneLODAssetSettings settings;
    if (const auto* assets = AssetManager::GetThreadCurrent())
        settings = BoneLODAssetSettings::Load(assets->GetRegistry(), GetPath());
    auto data = MakeShared<ModelBoneLOD>();
    if (!data->Build(*this, *skeleton, settings))
        Logger::Log::Warning("ModelAsset[{}]: {}", GetName(), data->Error());
    m_BoneLOD = std::move(data);
}

} // namespace GameEngine
