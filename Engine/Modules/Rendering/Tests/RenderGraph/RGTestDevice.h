#pragma once

// Test helper: create a real headless device the same way the existing render-graph
// tests do (e.g. RenderGraphAutoCompileBehaviorTests). RenderGraph's device-touching tests
// run against the REAL IDevice (not a mock) so there is no production disconnect.
// Returns null if no device can be created (e.g. no GPU) — callers GTEST_SKIP.

#include "Rendering/Core/Device.h"

#include <memory>
#include <string>

namespace GameEngine::Rendering::RenderGraph::Test
{

inline std::unique_ptr<IDevice> MakeHeadlessDevice(const std::string& pipelineCacheDirectory = {})
{
    DeviceDesc descriptor{};
    descriptor.pipelineCacheDirectory = pipelineCacheDirectory;
    descriptor.preferredAPI = GraphicsAPI::Vulkan;
    descriptor.enableDynamicRendering = true;
    descriptor.enableSwapchain = false; // headless: no window/surface in the test harness
    auto device = DeviceFactory::CreateDevice(descriptor);
    if (!device || !device->Initialize(descriptor))
        return nullptr;
    return device;
}

} // namespace GameEngine::Rendering::RenderGraph::Test
