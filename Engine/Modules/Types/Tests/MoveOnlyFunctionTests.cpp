#include "Types/MoveOnlyFunction.h"

#include "Memory/AllocationCountScope.h"

#include <array>
#include <functional>
#include <memory>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
// Counts the destructions of the callables that a test stores.
struct DestructionCounter
{
    int* Destroyed;
    explicit DestructionCounter(int* destroyed) noexcept : Destroyed(destroyed) {}
    DestructionCounter(DestructionCounter&& other) noexcept : Destroyed(other.Destroyed) { other.Destroyed = nullptr; }
    DestructionCounter(const DestructionCounter&) = delete;
    ~DestructionCounter()
    {
        if (Destroyed)
            ++*Destroyed;
    }
    int operator()(int value) const { return value + 1; }
};

int Twice(int value)
{
    return value * 2;
}
} // namespace

// The reason the type exists: a callback that captures a reference-counted handle, as an asset read's
// callbacks capture their fetch holder. libstdc++'s std::function puts such a lambda on the heap.
TEST(MoveOnlyFunctionTest, ALambdaThatCapturesASharedHandleIsStoredWithoutAllocating)
{
    auto handle = std::make_shared<int>(41);
    const Memory::AllocationCountScope probe(Memory::CountWindow::ThisThread);
    {
        MoveOnlyFunction<int()> read = [handle] { return *handle + 1; };
        MoveOnlyFunction<int()> moved = std::move(read);
        EXPECT_EQ(moved(), 42);
    }
    EXPECT_EQ(probe.Count(), 0u) << "storing, moving, calling and destroying the callback allocated";
    EXPECT_EQ(handle.use_count(), 1) << "the stored copy of the handle was released";
}

TEST(MoveOnlyFunctionTest, AMoveOnlyCaptureMovesWithTheFunction)
{
    MoveOnlyFunction<int()> first = [owned = std::make_unique<int>(7)] { return *owned; };
    MoveOnlyFunction<int()> second = std::move(first);
    EXPECT_FALSE(first) << "a moved-from function is empty";
    ASSERT_TRUE(second);
    EXPECT_EQ(second(), 7);
}

TEST(MoveOnlyFunctionTest, ACallableLargerThanTheInlineBufferIsStoredOnTheHeapAndStillCalled)
{
    std::array<int, 32> values{};
    values[31] = 5;
    static_assert(sizeof(values) > MoveOnlyFunction<int()>::kInlineBytes);
    const Memory::AllocationCountScope probe(Memory::CountWindow::ThisThread);
    MoveOnlyFunction<int()> large = [values] { return values[31]; };
    EXPECT_EQ(probe.Count(), 1u) << "a large callable takes one heap block";
    MoveOnlyFunction<int()> moved = std::move(large);
    EXPECT_EQ(moved(), 5);
    EXPECT_EQ(probe.Count(), 1u) << "moving a heap callable moves its pointer";
}

TEST(MoveOnlyFunctionTest, TheStoredCallableIsDestroyedOnceInPlaceAndOnTheHeap)
{
    int destroyed = 0;
    {
        MoveOnlyFunction<int(int)> inPlace = DestructionCounter(&destroyed);
        MoveOnlyFunction<int(int)> moved = std::move(inPlace);
        EXPECT_EQ(moved(1), 2);
    }
    EXPECT_EQ(destroyed, 1);

    destroyed = 0;
    {
        std::array<char, 64> padding{};
        MoveOnlyFunction<int(int)> onHeap = [counter = DestructionCounter(&destroyed), padding](int value) {
            return counter(value) + padding[0];
        };
        MoveOnlyFunction<int(int)> moved = std::move(onHeap);
        moved = nullptr;
        EXPECT_FALSE(moved);
    }
    EXPECT_EQ(destroyed, 1);
}

TEST(MoveOnlyFunctionTest, AnEmptyFunctionPointerOrStdFunctionMakesAnEmptyFunction)
{
    int (*none)(int) = nullptr;
    EXPECT_FALSE(MoveOnlyFunction<int(int)>(none));
    EXPECT_FALSE(MoveOnlyFunction<int(int)>(std::function<int(int)>()));
    EXPECT_FALSE(MoveOnlyFunction<int(int)>());

    MoveOnlyFunction<int(int)> pointer = &Twice;
    ASSERT_TRUE(pointer);
    EXPECT_EQ(pointer(4), 8);
}

TEST(MoveOnlyFunctionTest, AConstFunctionCallsItsCallableWithItsOwnState)
{
    const MoveOnlyFunction<int()> counter = [calls = 0]() mutable { return ++calls; };
    EXPECT_EQ(counter(), 1);
    EXPECT_EQ(counter(), 2) << "the callable keeps its state between calls, as std::function does";
}
