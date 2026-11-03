#include "Localization/LocalizationAsset.h"
#include "Logger/Logger.h"
#include "AssetCore/SharedFileRead.h"
#include <sstream>

namespace GameEngine::Localization
{
bool LocalizationAsset::Load()
{
    Vector<uint8> data;
    if (!ReadFileBytesShared(GetPath(), data))
    {
        Logger::Log::Error("Could not read localization asset {}", GetPath().string());
        if (!IsLoaded()) SetState(AssetState::Failed);
        return false;
    }
    return LoadFromData(data);
}

bool LocalizationAsset::LoadFromData(const Vector<uint8>& data)
{
    std::string document(data.begin(), data.end());
    std::istringstream input(document);
    LocalizationCatalog candidate;
    std::string error;
    if (!candidate.Load(input, error))
    {
        Logger::Log::Error("Localization asset {}: {}", GetPath().string(), error);
        if (!IsLoaded()) SetState(AssetState::Failed);
        return false;
    }
    m_Document = std::move(document);
    SetState(AssetState::Loaded);
    return true;
}

std::shared_ptr<LocalizationCatalog> LocalizationAsset::CreateCatalog(std::string& error) const
{
    if (!IsLoaded())
    {
        error = "Localization asset is not loaded.";
        return {};
    }
    auto catalog = std::make_shared<LocalizationCatalog>();
    std::istringstream input(m_Document);
    if (!catalog->Load(input, error)) return {};
    return catalog;
}

void LocalizationAsset::Unload()
{
    m_Document.clear();
    SetState(AssetState::Unloaded);
}

bool LocalizationAsset::AdoptReloadedPayload(Asset& staged)
{
    auto* candidate = dynamic_cast<LocalizationAsset*>(&staged);
    if (!candidate || !candidate->IsLoaded()) return false;
    m_Document.swap(candidate->m_Document);
    return true;
}
}
