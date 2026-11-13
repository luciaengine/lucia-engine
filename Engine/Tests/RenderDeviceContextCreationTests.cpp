// RenderDeviceContext::BeginDeviceCreation: the device is created on a job, and
// the thread that calls Initialize joins it, owns it and reports its failure.

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderDeviceContext.h"
#include "EngineLogCapture.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using GameEngine::Engine::Renderer::RenderDeviceContext;
using namespace GameEngine::Rendering;

constexpr size_t kPoolWorkerCount = 2;
constexpr size_t kPoolBlockingThreadBudget = 1;
constexpr size_t kProbeBufferBytes = 256;

DeviceDesc MakeHeadlessDesc()
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableSwapchain = false;
    desc.enableDebugLayer = false;
    return desc;
}

// A backend this build does not include, so the device factory refuses it.
constexpr GraphicsAPI kBackendNotInThisBuild =
#if defined(__APPLE__)
    GraphicsAPI::DirectX12;
#else
    GraphicsAPI::Metal;
#endif

const Logger::LogMessage* FindRecord(const std::vector<Logger::LogMessage>& records, std::string_view text)
{
    const auto found = std::find_if(records.begin(), records.end(), [text](const Logger::LogMessage& record)
                                    { return record.Message.find(text) != std::string::npos; });
    return found == records.end() ? nullptr : &*found;
}
} // namespace

TEST(RenderDeviceContextCreation, TheThreadThatInitializesOwnsADeviceCreatedOnAJob)
{
    constexpr std::string_view kCallingThreadMarker = "RenderDeviceContextCreation: the calling thread";
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount, kPoolBlockingThreadBudget);
    JobSystem::JobChannel channel(pool, JobSystem::JobChannelDesc{.Name = "Test device creation", .MaxRunning = 1});
    std::vector<Logger::LogMessage> records;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&records, Logger::LogLevel::Info);
        Logger::Log::Info("{}", kCallingThreadMarker);

        RenderDeviceContext context;
        context.BeginDeviceCreation(channel, MakeHeadlessDesc());
        if (!context.Initialize(RenderDeviceContext::InitParams{}))
            GTEST_SKIP() << "No Vulkan device available";
        IDevice* device = context.GetDevice();
        ASSERT_NE(device, nullptr);

        // A deferred destroy drained by WaitForIdle reaches the immediate destroy, which
        // asserts that it runs on the device's owner thread.
        BufferDesc bufferDesc{};
        bufferDesc.size = kProbeBufferBytes;
        bufferDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
        bufferDesc.debugName = "RenderDeviceContextCreation probe";
        const BufferHandle buffer = device->CreateBuffer(bufferDesc);
        ASSERT_TRUE(buffer.IsValid());
        device->DestroyBuffer(buffer);
        device->WaitForIdle();
        context.Shutdown();
        Logger::Log::Flush();
    }

    const Logger::LogMessage* marker = FindRecord(records, kCallingThreadMarker);
    ASSERT_NE(marker, nullptr) << "The capture saw no line of the calling thread";
    const Logger::LogMessage* created = FindRecord(records, "[Startup]   Device create+init");
    ASSERT_NE(created, nullptr) << "The device creation logged no timing line";
    EXPECT_NE(std::string(created->ThreadId), std::string(marker->ThreadId))
        << "The device was created on the thread that called Initialize, not on the job";
}

TEST(RenderDeviceContextCreation, AFailedCreationOnAJobReportsTheFixWhereInitializeJoinsIt)
{
    constexpr std::string_view kCallingThreadMarker = "RenderDeviceContextCreation: the joining thread";
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount, kPoolBlockingThreadBudget);
    JobSystem::JobChannel channel(pool, JobSystem::JobChannelDesc{.Name = "Test device creation", .MaxRunning = 1});
    DeviceDesc desc = MakeHeadlessDesc();
    desc.preferredAPI = kBackendNotInThisBuild;

    std::vector<Logger::LogMessage> records;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&records, Logger::LogLevel::Info);
        Logger::Log::Info("{}", kCallingThreadMarker);
        RenderDeviceContext context;
        context.BeginDeviceCreation(channel, desc);
        EXPECT_FALSE(context.Initialize(RenderDeviceContext::InitParams{}));
        EXPECT_EQ(context.GetDevice(), nullptr);
        Logger::Log::Flush();
    }

    const Logger::LogMessage* marker = FindRecord(records, kCallingThreadMarker);
    ASSERT_NE(marker, nullptr) << "The capture saw no line of the joining thread";
    const Logger::LogMessage* fix = FindRecord(records, "Request a graphics API that this build includes");
    ASSERT_NE(fix, nullptr) << "Initialize returned false without the fix";
    EXPECT_EQ(fix->Level, Logger::LogLevel::Error);
    EXPECT_EQ(std::string(fix->ThreadId), std::string(marker->ThreadId))
        << "The fix was not reported on the thread that joined the creation";
    EXPECT_EQ(FindRecord(records, "Install or update the GPU driver"), nullptr)
        << "A backend that is not in the build was reported as a driver problem";
}

TEST(RenderDeviceContextCreation, InitializeAfterAFailedCreationUsesItsOwnDeviceDesc)
{
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount, kPoolBlockingThreadBudget);
    JobSystem::JobChannel channel(pool, JobSystem::JobChannelDesc{.Name = "Test device creation", .MaxRunning = 1});
    DeviceDesc failingDesc = MakeHeadlessDesc();
    failingDesc.preferredAPI = kBackendNotInThisBuild;

    RenderDeviceContext context;
    context.BeginDeviceCreation(channel, failingDesc);
    ASSERT_FALSE(context.Initialize(RenderDeviceContext::InitParams{}));

    // The failed creation's description names a backend this build does not include,
    // so only params.deviceDesc can create a device here.
    RenderDeviceContext::InitParams params{};
    params.deviceDesc = MakeHeadlessDesc();
    std::vector<Logger::LogMessage> records;
    bool initialized = false;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&records, Logger::LogLevel::Info);
        initialized = context.Initialize(params);
        Logger::Log::Flush();
    }
    EXPECT_EQ(FindRecord(records, "Request a graphics API that this build includes"), nullptr)
        << "Initialize created the device from the failed creation's description";
    if (!initialized)
        GTEST_SKIP() << "No Vulkan device available";
    EXPECT_NE(context.GetDevice(), nullptr);
    context.Shutdown();
}

TEST(RenderDeviceContextCreation, AContextDestroyedBeforeInitializeWaitsForItsDeviceJob)
{
    JobSystem::WorkStealingThreadPool pool(kPoolWorkerCount, kPoolBlockingThreadBudget);
    JobSystem::JobChannel channel(pool, JobSystem::JobChannelDesc{.Name = "Test device creation", .MaxRunning = 1});
    // The job writes into the context, so the destructor waits for it; GetDevice asserts if it does not.
    RenderDeviceContext context;
    context.BeginDeviceCreation(channel, MakeHeadlessDesc());
}
