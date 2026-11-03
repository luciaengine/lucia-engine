#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "Physics/PhysicsMaterial.h"

#include <filesystem>

namespace GameEngine::PhysicsECS
{

/**
 * @brief An authored contact surface (.physmat): friction, restitution and the
 * combine modes that resolve them against whatever it touches.
 *
 * The asset layer is deliberately the only place assets and physics meet — the
 * Physics module resolves materials by index and never sees a GUID or a file.
 */
class PhysicsMaterialAsset : public ::GameEngine::Asset
{
public:
    static constexpr int32 kSchemaVersion = 1;

    PhysicsMaterialAsset(const ::GameEngine::GUID& guid, const std::filesystem::path& path)
        : ::GameEngine::Asset(guid, ::GameEngine::AssetType::PhysicsMaterial, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    bool SaveToData(Vector<uint8>& outData) const;

    const Physics::PhysicsMaterial& Material() const { return m_Material; }
    Physics::PhysicsMaterial& Material() { return m_Material; }

private:
    bool ParseJson(const std::string& text);
    std::string SerializeJson() const;

    Physics::PhysicsMaterial m_Material{};
};

} // namespace GameEngine::PhysicsECS
