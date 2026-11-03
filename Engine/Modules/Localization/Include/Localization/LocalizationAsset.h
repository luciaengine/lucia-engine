#pragma once

#include "AssetCore/Asset.h"
#include "Localization/LocalizationCatalog.h"

namespace GameEngine::Localization
{
// A validated catalog document. Each UI context creates its own locale state.
class LocalizationAsset final : public Asset
{
public:
    LocalizationAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Localization, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;
    std::shared_ptr<LocalizationCatalog> CreateCatalog(std::string& error) const;

protected:
    bool ReloadFromData(const Vector<uint8>& data) override { return LoadFromData(data); }
    bool AdoptReloadedPayload(Asset& staged) override;

private:
    std::string m_Document;
};
}
