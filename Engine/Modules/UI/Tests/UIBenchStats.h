#pragma once

// What the UI benches report for a run of timing samples, and how they print it:
// one statistics rule and one table layout for every bench, so two benches'
// numbers mean the same thing.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace GameEngine::UITesting
{

// NDEBUG is the only build signal a test sees: DebugFast keeps asserts, so it
// reports as "not Release" together with Debug.
#ifdef NDEBUG
inline constexpr const char* kBenchConfig = "Release";
#else
inline constexpr const char* kBenchConfig = "not Release";
#endif

using BenchClock = std::chrono::steady_clock;

inline double MillisecondsSince(BenchClock::time_point start)
{
    return std::chrono::duration<double, std::milli>(BenchClock::now() - start).count();
}

// The nearest-rank percentile `p` (0 to 1) of samples sorted ascending: the
// smallest sample at least a fraction `p` of the samples do not exceed.
inline double Percentile(const std::vector<double>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    const double rank = std::ceil(p * static_cast<double>(sorted.size()));
    const size_t index = rank < 1.0 ? 0 : static_cast<size_t>(rank) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

struct BenchStats
{
    double Median = 0.0;
    double P95 = 0.0;
    double Mean = 0.0;
    double Min = 0.0;
    double Max = 0.0;
    size_t Count = 0;
};

inline BenchStats Summarize(std::vector<double> samples)
{
    BenchStats stats{};
    if (samples.empty())
        return stats;
    std::sort(samples.begin(), samples.end());
    stats.Count = samples.size();
    stats.Median = samples[samples.size() / 2];
    stats.P95 = Percentile(samples, 0.95);
    stats.Min = samples.front();
    stats.Max = samples.back();
    double sum = 0.0;
    for (double sample : samples)
        sum += sample;
    stats.Mean = sum / static_cast<double>(samples.size());
    return stats;
}

// "=== <title> | config=<kBenchConfig><details> ===", then the column names of
// PrintBenchRow. `details` is the bench's own " | key=value" pairs.
inline void PrintBenchHeader(const char* title, const std::string& details = {})
{
    std::printf("\n=== %s | config=%s%s ===\n", title, kBenchConfig, details.c_str());
    std::printf("  %-40s  %9s  %9s  %9s  %9s  %9s   %s\n", "metric", "median", "p95", "mean", "min", "max",
                "n");
}

inline void PrintBenchRow(const char* label, const BenchStats& stats)
{
    std::printf("  %-40s  %9.4f  %9.4f  %9.4f  %9.4f  %9.4f   %zu\n", label, stats.Median, stats.P95, stats.Mean,
                stats.Min, stats.Max, stats.Count);
}

} // namespace GameEngine::UITesting
