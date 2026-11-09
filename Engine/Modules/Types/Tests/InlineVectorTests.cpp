#include "Types/InlineVector.h"

#include "Memory/AllocationCountScope.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

using GameEngine::InlineVector;
using GameEngine::Memory::AllocationCountScope;
using GameEngine::Memory::CountWindow;

namespace
{

template <typename T>
concept StorableInInlineVector = requires { typename InlineVector<T, 4>; };

// Padded to its alignment by hand, so no compiler pads it (MSVC reports implicit padding as C4324).
struct alignas(64) CacheLineValue
{
    std::uint64_t Word = 0;
    std::byte Padding[56] = {};
};

static_assert(StorableInInlineVector<int>);
static_assert(!StorableInInlineVector<std::string>, "elements are copied with memcpy and never destroyed");
static_assert(!StorableInInlineVector<CacheLineValue>, "the spill block comes from the default operator new");

template <typename T, std::size_t N>
bool HoldsElementsInline(const InlineVector<T, N>& vector)
{
    const auto* object = reinterpret_cast<const std::byte*>(&vector);
    const auto* first = reinterpret_cast<const std::byte*>(vector.begin());
    return first >= object && first < object + sizeof(vector);
}

std::vector<int> Copied(std::span<const int> values)
{
    return {values.begin(), values.end()};
}

void AddOne(std::span<int> values)
{
    for (int& value : values)
        ++value;
}

InlineVector<int, 4> Filled(int count)
{
    InlineVector<int, 4> values;
    for (int value = 0; value < count; ++value)
        values.PushBack(value);
    return values;
}

// Spills an InlineVector<int, N>, then counts the blocks PushBack allocates while it fills to 2N
// (none), to 4N (one) and past 4N (two). N = 4 and N = 8 are the sizes the engine's sites use.
template <std::size_t N>
void ExpectTheFirstSpillToHoldTwiceNThenDouble()
{
    InlineVector<int, N> values;
    while (values.Size() <= N)
        values.PushBack(0);

    AllocationCountScope allocations(CountWindow::ThisThread);
    while (values.Size() < 2 * N)
        values.PushBack(0);
    const std::uint64_t withinTheFirstBlock = allocations.Count();
    while (values.Size() < 4 * N)
        values.PushBack(0);
    const std::uint64_t withinTheSecondBlock = allocations.Count();
    values.PushBack(0);
    const std::uint64_t pastTheSecondBlock = allocations.Count();

    EXPECT_EQ(withinTheFirstBlock, 0u) << "N = " << N << ": the first spill holds 2N";
    EXPECT_EQ(withinTheSecondBlock, 1u) << "N = " << N << ": the second block holds 4N";
    EXPECT_EQ(pastTheSecondBlock, 2u) << "N = " << N;
}

} // namespace

TEST(InlineVectorTests, NothingAllocatesUntilTheElementPastN)
{
    AllocationCountScope allocations(CountWindow::ThisThread);
    InlineVector<int, 4> values;
    for (int value = 0; value < 4; ++value)
        values.PushBack(value);
    const std::uint64_t atN = allocations.Count();
    const bool inlineAtN = HoldsElementsInline(values);

    values.PushBack(4);
    const std::uint64_t pastN = allocations.Count();

    EXPECT_EQ(atN, 0u) << "N elements live inside the object";
    EXPECT_TRUE(inlineAtN);
    EXPECT_EQ(pastN, 1u) << "the spill is one operator new, seen by the allocation counter";
    EXPECT_FALSE(HoldsElementsInline(values));
    EXPECT_EQ(Copied(values), (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(InlineVectorTests, TheFirstSpillHoldsTwiceNAndTheBlockThenDoubles)
{
    ExpectTheFirstSpillToHoldTwiceNThenDouble<4>();
    ExpectTheFirstSpillToHoldTwiceNThenDouble<8>();
}

TEST(InlineVectorTests, PushBackOfItsOwnElementSurvivesTheSpill)
{
    InlineVector<int, 4> values = Filled(4);
    values[0] = 42;
    values.PushBack(values[0]);
    EXPECT_EQ(Copied(values), (std::vector<int>{42, 1, 2, 3, 42}));
}

TEST(InlineVectorTests, PushBackOfItsOwnElementSurvivesTheRegrowthOfItsBlock)
{
    InlineVector<int, 4> values = Filled(8);
    values[0] = 42;
    values.PushBack(values[0]);
    EXPECT_EQ(Copied(values), (std::vector<int>{42, 1, 2, 3, 4, 5, 6, 7, 42}));
}

TEST(InlineVectorTests, ACopyIsIndependentAndAllocatesOnlyForASpilledSource)
{
    const InlineVector<int, 4> small = Filled(3);
    const InlineVector<int, 4> large = Filled(6);

    AllocationCountScope allocations(CountWindow::ThisThread);
    InlineVector<int, 4> smallCopy(small);
    const std::uint64_t afterSmallCopy = allocations.Count();
    InlineVector<int, 4> largeCopy(large);
    const std::uint64_t afterLargeCopy = allocations.Count();
    smallCopy[0] = 10;
    largeCopy[5] = 50;

    EXPECT_EQ(afterSmallCopy, 0u);
    EXPECT_EQ(afterLargeCopy, 1u);
    EXPECT_EQ(Copied(small), (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(Copied(smallCopy), (std::vector<int>{10, 1, 2}));
    EXPECT_EQ(Copied(large), (std::vector<int>{0, 1, 2, 3, 4, 5}));
    EXPECT_EQ(Copied(largeCopy), (std::vector<int>{0, 1, 2, 3, 4, 50}));
}

TEST(InlineVectorTests, CopyAssignmentReusesStorageThatFits)
{
    InlineVector<int, 4> target = Filled(8);
    const InlineVector<int, 4> fits = Filled(6);
    const InlineVector<int, 4> small = Filled(2);

    AllocationCountScope allocations(CountWindow::ThisThread);
    target = fits;
    target = small;
    const std::uint64_t count = allocations.Count();

    EXPECT_EQ(count, 0u) << "the target's 8-element block holds both sources";
    EXPECT_EQ(Copied(target), (std::vector<int>{0, 1}));
}

TEST(InlineVectorTests, MovingASpilledVectorTakesItsBlockAndLeavesTheSourceEmpty)
{
    InlineVector<int, 4> source = Filled(6);
    const int* block = source.begin();

    AllocationCountScope allocations(CountWindow::ThisThread);
    InlineVector<int, 4> moved(std::move(source));
    InlineVector<int, 4> assigned = Filled(2);
    assigned = std::move(moved);
    const std::uint64_t count = allocations.Count();

    EXPECT_EQ(count, 0u);
    EXPECT_EQ(assigned.begin(), block);
    EXPECT_EQ(Copied(assigned), (std::vector<int>{0, 1, 2, 3, 4, 5}));
    EXPECT_EQ(source.Size(), 0u);
    EXPECT_EQ(moved.Size(), 0u);
}

TEST(InlineVectorTests, MovingAnInlineVectorCopiesItsElementsAndTheTargetReleasesItsBlock)
{
    InlineVector<int, 4> source = Filled(3);
    InlineVector<int, 4> spilled = Filled(7);

    InlineVector<int, 4> moved(std::move(source));
    spilled = std::move(moved);

    EXPECT_TRUE(HoldsElementsInline(spilled)) << "a moved-to vector keeps nothing of its own, as std::vector";
    EXPECT_EQ(Copied(spilled), (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(source.Size(), 0u);
}

TEST(InlineVectorTests, AMovedFromVectorIsUsableAfterClearOrPushBack)
{
    InlineVector<int, 4> first = Filled(6);
    InlineVector<int, 4> second = Filled(2);
    InlineVector<int, 4> taken(std::move(first));
    InlineVector<int, 4> alsoTaken(std::move(second));

    first.Clear();
    first.PushBack(7);
    second.PushBack(8);

    EXPECT_EQ(Copied(first), (std::vector<int>{7}));
    EXPECT_TRUE(HoldsElementsInline(first)) << "the moved-from vector gave its block away";
    EXPECT_EQ(Copied(second), (std::vector<int>{8}));
}

TEST(InlineVectorTests, ResizeFillsNewElementsAndShrinks)
{
    InlineVector<int, 4> values;
    values.Resize(3, 9);
    EXPECT_EQ(Copied(values), (std::vector<int>{9, 9, 9}));
    values.Resize(1, 0);
    EXPECT_EQ(Copied(values), (std::vector<int>{9}));

    AllocationCountScope allocations(CountWindow::ThisThread);
    values.Resize(6, 1);
    const std::uint64_t count = allocations.Count();
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(Copied(values), (std::vector<int>{9, 1, 1, 1, 1, 1}));
}

TEST(InlineVectorTests, ReserveAllocatesOnceForThePushesThatFollow)
{
    InlineVector<int, 4> values;
    AllocationCountScope allocations(CountWindow::ThisThread);
    values.Reserve(3);
    const std::uint64_t withinN = allocations.Count();
    values.Reserve(10);
    for (int value = 0; value < 10; ++value)
        values.PushBack(value);
    const std::uint64_t pastN = allocations.Count();

    EXPECT_EQ(withinN, 0u);
    EXPECT_EQ(pastN, 1u);
    EXPECT_EQ(values.Size(), 10u);
}

TEST(InlineVectorTests, ClearKeepsTheHeapBlockForThePushesThatFollow)
{
    InlineVector<int, 4> values = Filled(8);
    AllocationCountScope allocations(CountWindow::ThisThread);
    values.Clear();
    for (int value = 0; value < 8; ++value)
        values.PushBack(value);
    const std::uint64_t count = allocations.Count();

    EXPECT_EQ(count, 0u) << "Clear keeps the storage, inline or heap";
    EXPECT_FALSE(HoldsElementsInline(values));
}

TEST(InlineVectorTests, IterationVisitsTheElementsInOrderInlineAndSpilled)
{
    for (int count : {3, 9})
    {
        InlineVector<int, 4> values = Filled(count);
        int expected = 0;
        for (int value : values)
            EXPECT_EQ(value, expected++);
        EXPECT_EQ(expected, count);
    }
}

TEST(InlineVectorTests, PassesToFunctionsAsASpan)
{
    InlineVector<int, 4> values = Filled(5);
    AddOne(values);
    const InlineVector<int, 4>& view = values;
    EXPECT_EQ(Copied(view), (std::vector<int>{1, 2, 3, 4, 5}));
}

#if GE_DEBUG_INSTRUMENTATION && !defined(NDEBUG)
// A comma inside EXPECT_DEATH's statement splits the macro argument, hence the alias.
using IntVector = InlineVector<int, 4>;

TEST(InlineVectorDeathTest, AnIndexPastSizeAsserts)
{
    EXPECT_DEATH(
        {
            IntVector values = Filled(2);
            values[2] = 1;
        },
        "InlineVector index past Size");
}

TEST(InlineVectorDeathTest, ReadingAMovedFromVectorAsserts)
{
    EXPECT_DEATH(
        {
            IntVector values = Filled(2);
            IntVector taken(std::move(values));
            int sum = 0;
            for (int value : values)
                sum += value;
            values.PushBack(sum);
        },
        "InlineVector read after it was moved from");
}

TEST(InlineVectorDeathTest, ACopyOfAMovedFromVectorIsStillMovedFrom)
{
    EXPECT_DEATH(
        {
            IntVector values = Filled(2);
            IntVector taken(std::move(values));
            IntVector copy(values);
            copy.begin();
        },
        "InlineVector read after it was moved from");
}
#endif
