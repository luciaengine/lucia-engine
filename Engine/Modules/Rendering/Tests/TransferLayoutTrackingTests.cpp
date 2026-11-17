#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Vulkan/VulkanDevice/VulkanDevice.h"

#include <cstdlib>
#include <cstring>
#include <memory>

using namespace GameEngine::Rendering;

// A barrier recorded outside the render graph takes its old layout from the image's flat tracker
// when its own command list has not touched the image. A transfer operation on an earlier list
// must move that tracker, or the barrier transitions from a layout the image is not in.
namespace
{
constexpr uint32_t kSize = 4;

std::unique_ptr<IDevice> CreateHeadlessDevice()
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    DeviceDesc desc{};
    desc.applicationName = "TransferLayoutTracking";
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        return nullptr;
    return device;
}

TextureHandle CreateSampledTexture(IDevice& device)
{
    TextureDesc desc{};
    desc.width = desc.height = kSize;
    desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) | static_cast<uint32_t>(TextureUsage::TransferDst);
    desc.debugName = "TransferLayoutTracking.Texture";
    return device.CreateTexture(desc);
}

void Submit(IDevice& device, CommandList& commands)
{
    commands.End();
    device.ExecuteCommandLists({&commands});
    device.WaitForIdle();
}

// Rests the texture in ShaderResource, as a sampled texture does between uploads.
void RestInShaderResource(IDevice& device, TextureHandle texture)
{
    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::Undefined, ResourceState::ShaderResource));
    Submit(device, *commands);
}
} // namespace

TEST(TransferLayoutTracking, ACopyIntoAnImageMovesItsTrackedLayout)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    auto* vulkan = dynamic_cast<VulkanDevice*>(device.get());
    if (!vulkan)
        GTEST_SKIP() << "Not running on the Vulkan backend";

    const TextureHandle texture = CreateSampledTexture(*device);
    ASSERT_TRUE(texture.IsValid());
    BufferDesc stagingDesc{};
    stagingDesc.size = kSize * kSize * 4u;
    stagingDesc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    stagingDesc.memoryUsage = BufferMemoryUsage::Upload;
    const BufferHandle staging = device->CreateBuffer(stagingDesc);
    ASSERT_TRUE(staging.IsValid());
    if (void* mapped = device->MapBuffer(staging))
        std::memset(mapped, 0xFF, stagingDesc.size);
    device->UnmapBuffer(staging);

    RestInShaderResource(*device, texture);
    ASSERT_EQ(vulkan->GetTrackedImageLayout(texture), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    auto upload = device->CreateCommandList(IDevice::QueueType::Graphics);
    upload->Begin();
    upload->CopyBufferToTextureSubresource(staging, texture, 0, 0, kSize, kSize);
    Submit(*device, *upload);
    EXPECT_EQ(vulkan->GetTrackedImageLayout(texture), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    device->DestroyBuffer(staging);
    device->DestroyTexture(texture);
}

TEST(TransferLayoutTracking, AClearOfAnImageMovesItsTrackedLayout)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    auto* vulkan = dynamic_cast<VulkanDevice*>(device.get());
    if (!vulkan)
        GTEST_SKIP() << "Not running on the Vulkan backend";

    const TextureHandle texture = CreateSampledTexture(*device);
    ASSERT_TRUE(texture.IsValid());
    RestInShaderResource(*device, texture);
    ASSERT_EQ(vulkan->GetTrackedImageLayout(texture), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    auto clear = device->CreateCommandList(IDevice::QueueType::Graphics);
    clear->Begin();
    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    clear->ClearColorImageSubresource(texture, 0, 0, black);
    Submit(*device, *clear);
    EXPECT_EQ(vulkan->GetTrackedImageLayout(texture), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    device->DestroyTexture(texture);
}
