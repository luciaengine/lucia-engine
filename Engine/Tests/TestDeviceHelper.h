#pragma once

#include "Rendering/Core/Device.h"
#include "../../Tests/TestTempDir.h"

#include <atomic>
#include <memory>

inline std::unique_ptr<GameEngine::Rendering::IDevice> CreateVulkanDeviceFast()
{
    GameEngine::Rendering::DeviceDesc desc{};
    desc.preferredAPI = GameEngine::Rendering::GraphicsAPI::Vulkan;
    desc.enableDebugLayer = false;
    desc.enableDynamicRendering = true;
    static const GameEngine::TestUtils::ScopedTempDir kCacheRoot{
        GameEngine::TestUtils::MakeUniqueTempDirectory("ge-render-tests")};
    static std::atomic<uint64_t> s_NextCache{0};
    desc.pipelineCacheDirectory = (kCacheRoot.Path() / std::to_string(s_NextCache.fetch_add(1))).string();

    auto device = GameEngine::Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        return nullptr;
    return device;
}
