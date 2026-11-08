#include <gtest/gtest.h>

#include "Rendering/Materials/ShaderCompileService.h"
#include "ScopedShaderCompileTarget.h"

#include <atomic>
#include <barrier>
#include <thread>

using namespace GameEngine::Rendering;

TEST(ShaderCompileTarget, PublishedTupleIsCapturedAsOneValue)
{
    GameEngine::TestSupport::ScopedShaderCompileTarget restore{CurrentShaderCompileTarget()};
    constexpr ShaderCompileTarget kFirst{SpirvTarget::Spirv_1_3, false, true};
    constexpr ShaderCompileTarget kSecond{SpirvTarget::Spirv_1_6, true, false};
    constexpr int kIterations = 100000;
    ApplyShaderCompileProfile(kFirst);
    std::atomic<bool> start{false};
    std::jthread writer([&start, first = kFirst, second = kSecond]
    {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int iteration = 0; iteration < kIterations; ++iteration)
        {
            ApplyShaderCompileProfile(first);
            ApplyShaderCompileProfile(second);
        }
    });
    start.store(true, std::memory_order_release);
    bool sawMixedTuple = false;
    for (int iteration = 0; iteration < kIterations; ++iteration)
    {
        const ShaderCompileTarget captured = CurrentShaderCompileTarget();
        sawMixedTuple = sawMixedTuple || (captured != kFirst && captured != kSecond);
    }
    writer.join();
    EXPECT_FALSE(sawMixedTuple);
}

TEST(ShaderCompileTarget, ConcurrentScalarSettersPreserveTheOtherFields)
{
    GameEngine::TestSupport::ScopedShaderCompileTarget restore{CurrentShaderCompileTarget()};
    constexpr int kIterations = 1000;
    std::barrier phase(4);
    std::jthread compatibility([&phase]
    {
        for (int iteration = 0; iteration < kIterations; ++iteration)
        {
            phase.arrive_and_wait();
            SetCompatShaderProfile(true);
            phase.arrive_and_wait();
        }
    });
    std::jthread interpolation([&phase]
    {
        for (int iteration = 0; iteration < kIterations; ++iteration)
        {
            phase.arrive_and_wait();
            SetInterpolationFunctionsAvailable(true);
            phase.arrive_and_wait();
        }
    });
    std::jthread spirv([&phase]
    {
        for (int iteration = 0; iteration < kIterations; ++iteration)
        {
            phase.arrive_and_wait();
            ShaderCompileService::SetMaxSupportedSpirv(SpirvTarget::Spirv_1_6);
            phase.arrive_and_wait();
        }
    });
    bool lostUpdate = false;
    for (int iteration = 0; iteration < kIterations; ++iteration)
    {
        ApplyShaderCompileProfile({SpirvTarget::Spirv_1_3, false, false});
        phase.arrive_and_wait();
        phase.arrive_and_wait();
        lostUpdate = lostUpdate || CurrentShaderCompileTarget() !=
            ShaderCompileTarget{SpirvTarget::Spirv_1_6, true, true};
    }
    compatibility.join();
    interpolation.join();
    spirv.join();
    EXPECT_FALSE(lostUpdate);
}
