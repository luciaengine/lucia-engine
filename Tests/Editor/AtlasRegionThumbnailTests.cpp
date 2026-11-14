#include "Thumbnails/SpriteFramesThumbnailHandler.h"
#include "Thumbnails/ThumbnailImageCache.h"
#include "Thumbnails/ThumbnailImageOps.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <future>
#include "JobSystem/WorkStealingThreadPool.h"
using namespace GameEngine;
TEST(AtlasRegionThumbnail, CropsSiblingRegionsAndCachesSeparately)
{
    const auto root = std::filesystem::temp_directory_path() / ("atlas-region-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    auto sheet = Editor::MakeTransparent(32,16);
    for (int y=0;y<16;++y) for (int x=0;x<32;++x) {
        const int i=(y*32+x)*4;
        sheet.Pixels[i+(x<16?0:2)]=255; sheet.Pixels[i+3]=255;
    }
    const auto source=root/"sheet.png";
    ASSERT_TRUE(Editor::WritePng(source,sheet));
    JobSystem::WorkStealingThreadPool jobs(2);
    SpriteFramesThumbnailHandler handler(jobs, nullptr);
    handler.SetCacheRoot(root);
    auto request=[&](std::array<float,4> uv) {
        auto ready=std::make_shared<std::promise<std::string>>(); auto future=ready->get_future();
        auto path=handler.GetOrRequestAtlasRegion(source,uv,[ready](const std::string& p){ready->set_value(p);});
        if (path.empty()) {
            if(future.wait_for(std::chrono::seconds(10))!=std::future_status::ready) return std::string();
            path=future.get();
        }
        return path;
    };
    const auto left=request({0,0,.5f,1}), right=request({.5f,0,1,1});
    ASSERT_FALSE(left.empty()); ASSERT_FALSE(right.empty()); EXPECT_NE(left,right);
    auto red=Editor::DecodeImage(left), blue=Editor::DecodeImage(right);
    ASSERT_FALSE(red.IsEmpty()); ASSERT_FALSE(blue.IsEmpty());
    auto center=[](const Editor::ThumbnailImage& p,int c){return p.Pixels[((p.Height/2)*p.Width+p.Width/2)*4+c];};
    EXPECT_EQ(center(red,0),255); EXPECT_EQ(center(red,2),0);
    EXPECT_EQ(center(blue,0),0); EXPECT_EQ(center(blue,2),255);
    EXPECT_EQ(request({0,0,.5f,1}),left);
    std::filesystem::remove_all(root);
}

TEST(AtlasRegionThumbnail, MaterialKeyRemovesMatteWithoutChangingUnkeyedSprites)
{
    const auto root = std::filesystem::temp_directory_path() / ("atlas-key-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    auto sheet=Editor::MakeTransparent(8,8);
    for (int i=0;i<64;++i) { sheet.Pixels[i*4]=255; sheet.Pixels[i*4+2]=255; sheet.Pixels[i*4+3]=255; }
    // Green corners hold the crop bounds while the central magenta hole is keyed.
    for (int i : {0,7,56,63}) { sheet.Pixels[i*4]=0; sheet.Pixels[i*4+1]=255; sheet.Pixels[i*4+2]=0; }
    const auto source=root/"key.png";
    ASSERT_TRUE(Editor::WritePng(source,sheet));
    JobSystem::WorkStealingThreadPool jobs(2);
    SpriteFramesThumbnailHandler handler(jobs, nullptr);handler.SetCacheRoot(root);
    auto request=[&](std::array<float,2> key) {
        auto promise=std::make_shared<std::promise<std::string>>();auto future=promise->get_future();
        auto path=handler.GetOrRequestAtlasRegion(source,{0,0,1,1},[promise](const std::string& p){promise->set_value(p);},key);
        if(path.empty()) {
            if(future.wait_for(std::chrono::seconds(10))!=std::future_status::ready)return std::string();
            path=future.get();
        }
        return path;
    };
    auto rawPath=request({-1,0}), keyedPath=request({.005f,1});
    ASSERT_FALSE(rawPath.empty());ASSERT_FALSE(keyedPath.empty());EXPECT_NE(rawPath,keyedPath);
    auto raw=Editor::DecodeImage(rawPath),keyed=Editor::DecodeImage(keyedPath);
    ASSERT_FALSE(raw.IsEmpty());ASSERT_FALSE(keyed.IsEmpty());
    auto alpha=[](const Editor::ThumbnailImage& p){return p.Pixels[((p.Height/2)*p.Width+p.Width/2)*4+3];};
    EXPECT_EQ(alpha(raw),255);EXPECT_EQ(alpha(keyed),0);
    std::filesystem::remove_all(root);
}

// Optional real-art acceptance capture, using the same worker and cache as the UI.
TEST(AtlasRegionThumbnail, AuthoredKeyedArtworkPreview)
{
    const char* source=std::getenv("GE_ATLAS_PREVIEW_SOURCE");
    if (!source) GTEST_SKIP() << "Set GE_ATLAS_PREVIEW_SOURCE for a real-art preview";
    const auto root=std::filesystem::temp_directory_path()/"ge-atlas-preview-qa";
    std::filesystem::create_directories(root);
    JobSystem::WorkStealingThreadPool jobs(2);
    SpriteFramesThumbnailHandler handler(jobs, nullptr);handler.SetCacheRoot(root);
    auto promise=std::make_shared<std::promise<std::string>>();auto future=promise->get_future();
    auto path=handler.GetOrRequestAtlasRegion(source,{0,0,1,1},[promise](const std::string& p){promise->set_value(p);},{.005f,1});
    if(path.empty()) {
        ASSERT_EQ(future.wait_for(std::chrono::seconds(20)),std::future_status::ready);
        path=future.get();
    }
    ASSERT_FALSE(path.empty());
    const auto preview=Editor::DecodeImage(path);
    ASSERT_FALSE(preview.IsEmpty());
    size_t opaque=0,transparent=0;
    for(size_t i=0;i+3<preview.Pixels.size();i+=4) {
        if(preview.Pixels[i+3]==0) { ++transparent; continue; }
        ++opaque;
        EXPECT_LE(float(std::min(preview.Pixels[i],preview.Pixels[i+2]))-preview.Pixels[i+1],.005f*255);
    }
    EXPECT_GT(opaque,0u); EXPECT_GT(transparent,0u);
    std::cout << "Preview: " << path << '\n';
}
