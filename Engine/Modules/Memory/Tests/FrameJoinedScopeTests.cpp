// The calling thread's frame-joined bit and the scope that is the one way to change it.

#include "Memory/FrameJoinedScope.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <thread>

#if GE_DEBUG_INSTRUMENTATION

namespace
{
using GameEngine::Memory::Detail::FrameJoinedScope;
using GameEngine::Memory::Detail::IsFrameJoined;

bool ReadOnAFreshThread()
{
    bool frameJoined = true;
    std::thread reader([&frameJoined] { frameJoined = IsFrameJoined(); });
    reader.join();
    return frameJoined;
}

void ThrowInsideAJoinedScope()
{
    const FrameJoinedScope joined(true);
    throw std::runtime_error("unit failed");
}
} // namespace

TEST(FrameJoinedScope, AThreadStartsNotFrameJoined)
{
    EXPECT_FALSE(ReadOnAFreshThread());
}

TEST(FrameJoinedScope, ScopesNestAndRestoreTheEnclosingValue)
{
    ASSERT_FALSE(IsFrameJoined());
    {
        const FrameJoinedScope joined(true);
        EXPECT_TRUE(IsFrameJoined());
        {
            const FrameJoinedScope cleared(false);
            EXPECT_FALSE(IsFrameJoined());
            {
                const FrameJoinedScope joinedAgain(true);
                EXPECT_TRUE(IsFrameJoined());
            }
            EXPECT_FALSE(IsFrameJoined());
        }
        EXPECT_TRUE(IsFrameJoined());
    }
    EXPECT_FALSE(IsFrameJoined());
}

TEST(FrameJoinedScope, AScopeRestoresTheBitWhenUnwound)
{
    ASSERT_FALSE(IsFrameJoined());
    EXPECT_THROW(ThrowInsideAJoinedScope(), std::runtime_error);
    EXPECT_FALSE(IsFrameJoined());
}

TEST(FrameJoinedScope, TheBitBelongsToTheCallingThread)
{
    const FrameJoinedScope joined(true);
    ASSERT_TRUE(IsFrameJoined());
    EXPECT_FALSE(ReadOnAFreshThread());
}

#endif
