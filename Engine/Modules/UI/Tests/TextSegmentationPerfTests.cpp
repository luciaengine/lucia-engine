// Word segmentation has to stay linear in the length of the text.
//
// The shape that broke it is a word character followed by a long run of
// combining marks — one paste of a "zalgo" string, or any decomposed text a
// clipboard can hold. Segmentation ran the class test at every code point, and
// each test walked back over every mark before it, so the cost went as the
// square of the run: a 16 KB run cost seconds per segmentation pass in a Debug
// build, on the thread that draws the editor.
//
// The scaling check is the machine-independent half, and the one whose margin
// does not shrink on a faster machine: the same work per byte is done at both
// sizes, so a linear implementation spends the same time on each and a
// quadratic one spends four times as long on the larger. Across both configs:
// 0.80 to 1.02 linear over 14 samples, 3.17 to 5.59 quadratic over 10. The 2.0
// criterion sits in the empty band between them.

#include "UI/TextSegmentation.h"

#include "TestThreadCpuTime.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

using namespace GameEngine;

namespace
{

constexpr size_t kSmallBytes = 4 * 1024;
constexpr size_t kLargeBytes = 16 * 1024;

// Enough repetitions that a linear implementation is timed against the clock
// rather than against noise — milliseconds in Debug, hundreds of microseconds
// in Release — and the same total byte count at both sizes so the two timings
// are directly comparable.
constexpr size_t kWorkBytes = 64 * 1024;

// CPU time budget for one pass over a 16 KB string of combining marks.
constexpr double kPassBudgetMs = 60.0;

// A linear implementation holds this at ~1.0 for a 4x size step; a quadratic one
// lands at ~4.0.
constexpr double kMaxCostGrowth = 2.0;

// COMBINING ACUTE ACCENT, the mark the reproduction used.
constexpr std::string_view kMark = "\xCC\x81";

std::string MarkRun(std::string_view prefix, size_t bytes, std::string_view suffix)
{
    std::string text(prefix);
    while (text.size() + kMark.size() + suffix.size() <= bytes)
        text += kMark;
    text += suffix;
    return text;
}

// The three entry points a text control reaches on a paste, a double-click and a
// Ctrl+arrow, each at the index that makes it scan the whole string.
double PassMs(std::string_view text, size_t passes)
{
    volatile size_t sink = 0;
    const double batchMs = GameEngine::UI::Tests::MeanThreadCpuMilliseconds([&sink, text, passes] {
        for (size_t pass = 0; pass < passes; ++pass)
        {
            sink += TextSegmentation::DoubleClickSelectionAt(text, text.size()).Begin;
            sink += TextSegmentation::NextBoundary(text, 0);
            sink += TextSegmentation::PrevBoundary(text, text.size());
        }
    });
    (void)sink;
    return batchMs / static_cast<double>(passes);
}

} // namespace

// A word character carrying a long run of marks is one word, and finding that
// out has to cost one pass over the marks rather than one per mark.
TEST(TextSegmentationPerfTests, ARunOfCombiningMarksScansInLinearTime)
{
    const std::string small = MarkRun("a", kSmallBytes, "");
    const std::string large = MarkRun("a", kLargeBytes, "");

    const double smallMs = PassMs(small, kWorkBytes / kSmallBytes);
    const double largeMs = PassMs(large, kWorkBytes / kLargeBytes);

    const double costGrowth = (largeMs / static_cast<double>(large.size())) /
                              (smallMs / static_cast<double>(small.size()));

    std::printf("\n--- TextSegmentation: 'a' + a run of U+0301 ---\n");
    std::printf("  %6zu bytes : %8.3f ms per pass\n", small.size(), smallMs);
    std::printf("  %6zu bytes : %8.3f ms per pass  (criterion <= %.1f)\n",
                large.size(), largeMs, kPassBudgetMs);
    std::printf("  cost per byte, large / small : %.2f  (criterion <= %.1f; quadratic is ~4)\n",
                costGrowth, kMaxCostGrowth);
    std::printf("---\n");

    // The whole string is one word either way; a scan that stopped early would
    // beat the budget for the wrong reason.
    const TextSegmentation::ByteRange selection =
        TextSegmentation::DoubleClickSelectionAt(large, large.size());
    EXPECT_EQ(selection.Begin, 0u);
    EXPECT_EQ(selection.End, large.size());

    EXPECT_LE(largeMs, kPassBudgetMs) << "segmenting a 16 KB run of combining marks exceeds budget";
    EXPECT_LE(costGrowth, kMaxCostGrowth) << "cost per byte grows with length: the scan is not linear";
}

// The same run attached to a joiner instead, which is the path that also asks
// what sits on either side of it. That answer must not be recomputed per mark.
TEST(TextSegmentationPerfTests, ARunOfCombiningMarksOnAJoinerScansInLinearTime)
{
    const std::string small = MarkRun("1.", kSmallBytes, "5");
    const std::string large = MarkRun("1.", kLargeBytes, "5");

    const double smallMs = PassMs(small, kWorkBytes / kSmallBytes);
    const double largeMs = PassMs(large, kWorkBytes / kLargeBytes);

    const double costGrowth = (largeMs / static_cast<double>(large.size())) /
                              (smallMs / static_cast<double>(small.size()));

    std::printf("\n--- TextSegmentation: \"1.\" + a run of U+0301 + \"5\" ---\n");
    std::printf("  %6zu bytes : %8.3f ms per pass\n", small.size(), smallMs);
    std::printf("  %6zu bytes : %8.3f ms per pass  (criterion <= %.1f)\n",
                large.size(), largeMs, kPassBudgetMs);
    std::printf("  cost per byte, large / small : %.2f  (criterion <= %.1f; quadratic is ~4)\n",
                costGrowth, kMaxCostGrowth);
    std::printf("---\n");

    EXPECT_LE(largeMs, kPassBudgetMs) << "segmenting a 16 KB run of marks on a joiner exceeds budget";
    EXPECT_LE(costGrowth, kMaxCostGrowth) << "cost per byte grows with length: the scan is not linear";
}
