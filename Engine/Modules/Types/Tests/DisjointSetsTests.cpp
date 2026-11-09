#include "Types/DisjointSets.h"

#include "Memory/AllocationCountScope.h"

#include <algorithm>
#include <array>
#include <vector>

#include <gtest/gtest.h>

using GameEngine::DisjointSets;
using GameEngine::uint32;
using GameEngine::Memory::AllocationCountScope;
using GameEngine::Memory::CountWindow;

namespace
{

struct ItemPair
{
    uint32 First = 0;
    uint32 Second = 0;
};

// The joins make the sets {1, 3, 5, 7}, {2, 4, 6}, {0}, and {8}.
constexpr std::array<ItemPair, 5> kPairs{{{5, 3}, {3, 7}, {1, 7}, {6, 2}, {2, 4}}};
constexpr std::array<uint32, 9> kSmallestItemOfSet{0, 1, 2, 1, 2, 1, 2, 1, 8};

} // namespace

TEST(DisjointSetsTests, EachItemStartsInASetOfItsOwn)
{
    DisjointSets sets(4);
    for (uint32 item = 0; item < 4; ++item)
        EXPECT_EQ(sets.Find(item), item);
}

TEST(DisjointSetsTests, JoinsChainIntoOneSetAndLeaveTheRestApart)
{
    DisjointSets sets(6);
    sets.Join(0, 1);
    sets.Join(2, 3);
    sets.Join(1, 3);
    sets.Join(4, 4);
    const uint32 root = sets.Find(0);
    for (const uint32 item : {1u, 2u, 3u})
        EXPECT_EQ(sets.Find(item), root);
    EXPECT_NE(sets.Find(4), root);
    EXPECT_NE(sets.Find(5), root);
    EXPECT_NE(sets.Find(4), sets.Find(5));
}

TEST(DisjointSetsTests, FindGivesTheSmallestItemOfTheSetForEveryJoinOrder)
{
    std::array<uint32, kPairs.size()> order{0, 1, 2, 3, 4};
    do
    {
        for (const bool swapped : {false, true})
        {
            DisjointSets sets(kSmallestItemOfSet.size());
            for (const uint32 pair : order)
            {
                if (swapped)
                    sets.Join(kPairs[pair].Second, kPairs[pair].First);
                else
                    sets.Join(kPairs[pair].First, kPairs[pair].Second);
            }
            for (uint32 item = 0; item < kSmallestItemOfSet.size(); ++item)
                ASSERT_EQ(sets.Find(item), kSmallestItemOfSet[item]) << "item " << item << ", swapped " << swapped;
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(DisjointSetsTests, FindGivesTheSameRootsAfterItHalvesThePaths)
{
    constexpr uint32 kCount = 64;
    DisjointSets sets(kCount);
    // Joins from the top down make two chains: the even items and the odd items.
    for (uint32 item = kCount - 1; item > 1; --item)
        sets.Join(item - 2, item);
    std::vector<uint32> firstRoots(kCount);
    for (uint32 item = kCount; item-- > 0;)
        firstRoots[item] = sets.Find(item);
    std::vector<uint32> secondRoots(kCount);
    for (uint32 item = 0; item < kCount; ++item)
        secondRoots[item] = sets.Find(item);
    EXPECT_EQ(firstRoots, secondRoots);
    for (uint32 item = 0; item < kCount; ++item)
        EXPECT_EQ(firstRoots[item], item % 2);
}

TEST(DisjointSetsTests, ResetSplitsEverySetAndKeepsTheStorage)
{
    constexpr uint32 kCount = 1024;
    DisjointSets sets;
    sets.Reset(kCount);
    for (uint32 item = 1; item < kCount; ++item)
        sets.Join(0, item);
    AllocationCountScope scope(CountWindow::ThisThread);
    sets.Reset(kCount / 2);
    sets.Reset(kCount);
    for (uint32 item = 0; item < kCount; ++item)
        ASSERT_EQ(sets.Find(item), item);
    EXPECT_EQ(scope.Count(), 0u);
}

TEST(DisjointSetsTests, FindOnAChainOfAMillionItemsDoesNotOverflowTheStack)
{
    constexpr uint32 kCount = 1u << 20;
    DisjointSets sets(kCount);
    // Joins from the top down make one chain: each item links to the item below it.
    for (uint32 item = kCount - 1; item > 0; --item)
        sets.Join(item - 1, item);
    EXPECT_EQ(sets.Find(kCount - 1), 0u);
}
