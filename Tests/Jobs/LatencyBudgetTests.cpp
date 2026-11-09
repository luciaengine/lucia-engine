// The load verdict of the latency budget tests, from given loads: no real load is needed.

#include "LatencyBudget.h"

#include <gtest/gtest.h>

#include <string>

namespace GameEngine::Tests {

TEST(LatencyBudgetTest, AQuietMachineKeepsTheStrictBudget)
{
    EXPECT_EQ(DecideLatencyBudget(0.0).Verdict, LatencyBudgetVerdict::Strict);
    const LatencyBudget atLine = DecideLatencyBudget(kWidenAboveLoad);
    EXPECT_EQ(atLine.Verdict, LatencyBudgetVerdict::Strict);
    EXPECT_DOUBLE_EQ(atLine.Scale(30.0), 30.0);
}

TEST(LatencyBudgetTest, ABusyMachineWidensTheBudget)
{
    const LatencyBudget budget = DecideLatencyBudget(0.7);
    EXPECT_EQ(budget.Verdict, LatencyBudgetVerdict::Widen);
    EXPECT_DOUBLE_EQ(budget.Scale(30.0), 30.0 * kWidenedBudgetFactor);
    EXPECT_EQ(DecideLatencyBudget(kSkipAboveLoad).Verdict, LatencyBudgetVerdict::Widen);
}

TEST(LatencyBudgetTest, ASaturatedMachineSkipsTheBudget)
{
    EXPECT_EQ(DecideLatencyBudget(0.95).Verdict, LatencyBudgetVerdict::Skip);
    EXPECT_EQ(DecideLatencyBudget(1.5).Verdict, LatencyBudgetVerdict::Skip);
}

TEST(LatencyBudgetTest, TheMessageNamesTheLoadAndTheThresholds)
{
    const std::string text = DecideLatencyBudget(1.25).Describe();
    EXPECT_NE(text.find("1.25"), std::string::npos) << text;
    EXPECT_NE(text.find("0.50"), std::string::npos) << text;
    EXPECT_NE(text.find("0.90"), std::string::npos) << text;
    EXPECT_NE(text.find("not asserted"), std::string::npos) << text;
}

} // namespace GameEngine::Tests
