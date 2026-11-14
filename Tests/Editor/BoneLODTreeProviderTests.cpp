#include "Inspectors/BoneLOD/BoneLODTreeProvider.h"

#include "Animation/Skeleton/SkeletonData.h"

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
Animation::SkeletonData Hierarchy()
{
    Animation::SkeletonData skeleton;
    skeleton.BoneCount = 5u;
    skeleton.Parent = {-1, 0, 1, -1, 3};
    skeleton.BoneNames = {"Root", "a/b", "", "Root", "a/b"};
    skeleton.RestLocalMatrix.assign(5u * 16u, 0.0f);
    skeleton.InverseBind.assign(5u * 16u, 0.0f);
    for (uint32 node = 0u; node < 5u; ++node)
    {
        for (uint32 diagonal = 0u; diagonal < 4u; ++diagonal)
        {
            skeleton.RestLocalMatrix[node * 16u + diagonal * 5u] = 1.0f;
            skeleton.InverseBind[node * 16u + diagonal * 5u] = 1.0f;
        }
    }
    skeleton.ComputeTopologicalSort();
    return skeleton;
}
}

TEST(BoneLODTreeProvider, PreservesStructuredNamesAndDistinctRowsForAmbiguousPaths)
{
    const auto skeleton = Hierarchy();
    Editor::BoneLODTreeProvider tree(skeleton);
    ASSERT_EQ(tree.GetRootCount(), 2);
    EXPECT_EQ(tree.GetRootId(0), 1u);
    EXPECT_EQ(tree.GetRootId(1), 4u);
    EXPECT_EQ(tree.GetChildId(1u, 0), 2u);
    EXPECT_EQ(tree.Path(3u), (std::vector<std::string>{"Root", "a/b", ""}));
    EXPECT_STREQ(tree.GetLabel(3u), "(unnamed)");
    EXPECT_EQ(tree.Path(2u), tree.Path(5u));
    EXPECT_TRUE(tree.Path(0u).empty());
    EXPECT_TRUE(tree.Path(99u).empty());
}

TEST(BoneLODTreeProvider, SearchRetainsAncestorsAndCanRestoreTheWholeHierarchy)
{
    const auto skeleton = Hierarchy();
    Editor::BoneLODTreeProvider tree(skeleton);
    tree.SetSearch("A/B");
    EXPECT_TRUE(tree.IsVisible(1u));
    EXPECT_TRUE(tree.IsVisible(2u));
    EXPECT_FALSE(tree.IsVisible(3u));
    EXPECT_TRUE(tree.IsVisible(4u));
    EXPECT_TRUE(tree.IsVisible(5u));
    tree.SetSearch("no match");
    for (uint64 id = 1u; id <= 5u; ++id)
        EXPECT_FALSE(tree.IsVisible(id));
    tree.SetSearch("");
    for (uint64 id = 1u; id <= 5u; ++id)
        EXPECT_TRUE(tree.IsVisible(id));
}

TEST(BoneLODTreeProvider, SnapshotSurvivesModelPayloadReplacement)
{
    auto skeleton = Hierarchy();
    Editor::BoneLODTreeProvider tree(skeleton);
    skeleton = Animation::SkeletonData{};
    EXPECT_EQ(tree.Path(3u), (std::vector<std::string>{"Root", "a/b", ""}));
    EXPECT_EQ(tree.GetChildCount(4u), 1);
}

TEST(BoneLODTreeProvider, InvalidTopologyDoesNotExposeSelectableRows)
{
    auto skeleton = Hierarchy();
    skeleton.Parent[1] = 1;
    Editor::BoneLODTreeProvider tree(skeleton);
    EXPECT_EQ(tree.GetRootCount(), 0);
    EXPECT_TRUE(tree.Path(1u).empty());
}
