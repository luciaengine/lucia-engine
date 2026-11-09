#pragma once

#include "TestPlatform.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace GameEngine::Tests
{
// Machine loads (ReadMachineLoad) that change a latency budget. Above
// kWidenAboveLoad fewer than half the hardware threads are free, so a woken
// worker can wait for a core. Above kSkipAboveLoad a latency measures the
// scheduler, not the pool; the Windows busy share stops at 1, so the line is under 1.
constexpr double kWidenAboveLoad = 0.5;
constexpr double kSkipAboveLoad = 0.9;
// A widened budget allows two runnable threads on each hardware thread.
constexpr double kWidenedBudgetFactor = 2.0;

// The sleep backstop of the lost-wakeup stresses. A task whose wakeup is lost
// waits for a sleeper's backstop.
constexpr uint32_t kStressBackstopUs = 500000;
// A stress sample at or above this line is a lost wakeup at any machine load.
// A lost wakeup waits for a sleeper's backstop; a delay from a loaded scheduler stays under the line.
constexpr double kLostWakeupLineUs = 0.9 * kStressBackstopUs;
// A stress sample above this line fails on a quiet machine; the load verdict scales it.
constexpr double kStressLatencyLineUs = 100000.0;

enum class LatencyBudgetVerdict
{
    Strict,
    Widen,
    Skip,
};

// What a latency budget test does with its budget at the machine load it read.
struct LatencyBudget
{
    LatencyBudgetVerdict Verdict = LatencyBudgetVerdict::Strict;
    double Load = 0.0;

    // The budget to assert: the quiet budget, times kWidenedBudgetFactor unless the verdict is Strict.
    double Scale(double quietBudget) const
    {
        return Verdict == LatencyBudgetVerdict::Strict ? quietBudget : quietBudget * kWidenedBudgetFactor;
    }

    // Names the load, the thresholds and the verdict, for a skip or a failure message.
    std::string Describe() const
    {
        const char* verdict = Verdict == LatencyBudgetVerdict::Strict ? "strict budget"
                              : Verdict == LatencyBudgetVerdict::Widen ? "budget widened"
                                                                       : "budget not asserted";
        char text[192];
        std::snprintf(text, sizeof(text),
                      "machine load %.2f per hardware thread (widen x%.0f above %.2f, skip above %.2f): %s",
                      Load, kWidenedBudgetFactor, kWidenAboveLoad, kSkipAboveLoad, verdict);
        return text;
    }
};

// Strict up to kWidenAboveLoad, Widen up to kSkipAboveLoad, Skip above it.
inline LatencyBudget DecideLatencyBudget(double load)
{
    if (load > kSkipAboveLoad)
    {
        return {LatencyBudgetVerdict::Skip, load};
    }
    if (load > kWidenAboveLoad)
    {
        return {LatencyBudgetVerdict::Widen, load};
    }
    return {LatencyBudgetVerdict::Strict, load};
}

// Reads the machine load and decides. Call it before the test submits work to its pool.
inline LatencyBudget ReadLatencyBudget()
{
    return DecideLatencyBudget(ReadMachineLoad());
}

// The latency half of a test that also checks correctness: value <= the scaled budget.
// A Skip verdict asserts nothing: it prints the value with the load and records them as a test property.
inline void ExpectWithinBudget(double value, double quietBudget, const LatencyBudget& budget, const char* what)
{
    if (budget.Verdict == LatencyBudgetVerdict::Skip)
    {
        char valueText[64];
        std::snprintf(valueText, sizeof(valueText), "%.1f not asserted: ", value);
        const std::string record = valueText + budget.Describe();
        std::printf("[%s] %s\n", what, record.c_str());
        ::testing::Test::RecordProperty(what, record);
        return;
    }
    EXPECT_LE(value, budget.Scale(quietBudget)) << what << ": " << budget.Describe();
}
} // namespace GameEngine::Tests
