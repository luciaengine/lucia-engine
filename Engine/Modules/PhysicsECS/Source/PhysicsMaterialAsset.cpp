#include "PhysicsECS/PhysicsMaterialAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <string>

namespace GameEngine::PhysicsECS
{
namespace
{
using json = nlohmann::json;
using Physics::MaterialCombine;

// Combine modes serialize as names rather than ordinals so a .physmat diff
// reads as authoring intent, and so reordering the enum cannot silently
// repoint existing assets.
constexpr const char* kCombineNames[] = {"average", "minimum", "multiply", "maximum"};

const char* CombineName(MaterialCombine mode)
{
    const auto index = static_cast<size_t>(mode);
    return index < std::size(kCombineNames) ? kCombineNames[index] : kCombineNames[0];
}

MaterialCombine ParseCombine(const std::string& name)
{
    for (size_t i = 0; i < std::size(kCombineNames); ++i)
        if (name == kCombineNames[i])
            return static_cast<MaterialCombine>(i);
    return MaterialCombine::Average;
}

} // namespace

bool PhysicsMaterialAsset::Load()
{
    SetState(AssetState::Loading);
    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("PhysicsMaterialAsset: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool PhysicsMaterialAsset::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void PhysicsMaterialAsset::Unload()
{
    m_Material = Physics::PhysicsMaterial{};
    SetState(AssetState::Unloaded);
}

bool PhysicsMaterialAsset::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

bool PhysicsMaterialAsset::ParseJson(const std::string& text)
{
    try
    {
        const json doc = json::parse(text);

        Physics::PhysicsMaterial material{};
        const double friction = doc.value("friction", 0.5);
        const double restitution = doc.value("restitution", 0.1);
        if (!std::isfinite(friction) || !std::isfinite(restitution) ||
            friction > std::numeric_limits<float32>::max())
            throw std::invalid_argument("physics material coefficients must be finite float values");
        material.friction = static_cast<float32>(std::max(0.0, friction));
        material.restitution = static_cast<float32>(std::clamp(restitution, 0.0, 1.0));
        if (doc.contains("frictionCombine") && doc["frictionCombine"].is_string())
            material.frictionCombine = ParseCombine(doc["frictionCombine"].get<std::string>());
        if (doc.contains("restitutionCombine") && doc["restitutionCombine"].is_string())
            material.restitutionCombine = ParseCombine(doc["restitutionCombine"].get<std::string>());

        m_Material = material;
        SetState(AssetState::Loaded);
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("PhysicsMaterialAsset: failed to parse '{}': {}", GetPath().string(),
                           e.what());
        m_Material = Physics::PhysicsMaterial{};
        SetState(AssetState::Failed);
        return false;
    }
}

std::string PhysicsMaterialAsset::SerializeJson() const
{
    json doc;
    doc["schemaVersion"] = kSchemaVersion;
    doc["assetType"] = "PhysicsMaterial";
    doc["friction"] = m_Material.friction;
    doc["restitution"] = m_Material.restitution;
    // Omitted at the default so a diff shows only what was authored.
    if (m_Material.frictionCombine != MaterialCombine::Average)
        doc["frictionCombine"] = CombineName(m_Material.frictionCombine);
    if (m_Material.restitutionCombine != MaterialCombine::Average)
        doc["restitutionCombine"] = CombineName(m_Material.restitutionCombine);

    return doc.dump(2);
}

} // namespace GameEngine::PhysicsECS
