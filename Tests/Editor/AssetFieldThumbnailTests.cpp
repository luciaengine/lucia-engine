#include "UI/AssetField.h"

#include "Assets/AssetRegistry.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UI/UIElement.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <functional>
#include <memory>

using namespace GameEngine;

namespace
{
class DeferredThumbnailProvider final : public IThumbnailProvider
{
  public:
    std::function<void(const std::string&)> Ready;
    void SetAssetsRoot(const std::filesystem::path&) override {}
    std::string GetOrRequest(const std::filesystem::path&, int,
                             std::function<void(const std::string&)> onReady, bool) override
    {
        Ready = std::move(onReady);
        return {};
    }
};

class AssetFieldThumbnailTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        Root = std::filesystem::temp_directory_path() / ("ge-field-thumbnail-" + GUID::Generate().ToString());
        std::filesystem::create_directories(Root);
        ASSERT_TRUE(Registry.Initialize(Root));
        AssetMetadata meta{};
        meta.Guid = GUID::Generate();
        meta.Path = Root / "artwork.fbx";
        meta.Name = "Artwork";
        meta.Type = AssetType::Model;
        ASSERT_TRUE(Registry.RegisterAssetMetadata(meta));
        Value = meta.Guid;
        Field = std::make_unique<AssetField>();
        Field->SetAssetRegistry(&Registry);
        Field->SetThumbnailProvider(&Provider);
        Field->SetValue(Value);
        ASSERT_TRUE(static_cast<bool>(Provider.Ready));
    }
    void TearDown() override
    {
        Field.reset();
        Registry.Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
    }
    UIElement* Preview()
    {
        for (const auto& child : Field->GetChildren())
            if (child->HasClass("asset-field-preview"))
                return child.get();
        return nullptr;
    }
    std::filesystem::path Root;
    AssetRegistry Registry;
    GUID Value;
    DeferredThumbnailProvider Provider;
    std::unique_ptr<AssetField> Field;
};
} // namespace

TEST_F(AssetFieldThumbnailTest, ClearDiscardsPendingPreview)
{
    Field->Clear();
    Provider.Ready("cached.png");
    ASSERT_NE(Preview(), nullptr);
    EXPECT_TRUE(Preview()->HasClass("hidden"));
    EXPECT_TRUE(Field->GetValue().IsNull());
}

TEST_F(AssetFieldThumbnailTest, UnresolvedValueDiscardsPendingPreview)
{
    Field->SetValue(GUID::Generate());
    Provider.Ready("cached.png");
    ASSERT_NE(Preview(), nullptr);
    EXPECT_TRUE(Preview()->HasClass("hidden"));
}

TEST_F(AssetFieldThumbnailTest, RemovingProviderDiscardsPendingPreview)
{
    Field->SetThumbnailProvider(nullptr);
    Provider.Ready("cached.png");
    ASSERT_NE(Preview(), nullptr);
    EXPECT_TRUE(Preview()->HasClass("hidden"));
}

TEST_F(AssetFieldThumbnailTest, CompletionAfterFieldDestructionIsIgnored)
{
    Field.reset();
    EXPECT_NO_THROW(Provider.Ready("cached.png"));
}
