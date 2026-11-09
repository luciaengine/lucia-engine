// The registry scan's stages: a unit (256 files) that throws reaches the scan's
// future as that exception, through the stage's own promise, never as an
// abandoned promise. No stage holds a worker while the stage before it runs.

#include <gtest/gtest.h>

#include "Assets/AssetRegistry.h"
#include "Assets/ParserRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

using namespace GameEngine;

namespace
{

// The exception the probe parser throws, a type the scan has no handler of
// its own for.
class ScanProbeFailure : public std::runtime_error
{
  public:
    ScanProbeFailure() : std::runtime_error("scan probe failed on throws_here.scanprobe") {}
};

// Classifies .scanprobe files and throws while classifying one of them, as a
// parser sniffing a corrupt file might.
class ThrowingProbeParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::Unknown; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".scanprobe"}; }
    bool CanParse(const std::filesystem::path& filePath) const override
    {
        if (filePath.filename() == "throws_here.scanprobe")
            throw ScanProbeFailure();
        return true;
    }
    AssetParseResult Parse(const AssetMetadata&, AssetManager&) override { return AssetParseResult(false); }
    std::string GetName() const override { return "ThrowingProbeParser"; }
};

// Classifies .scanhold files. The first call holds its thread until Open, so
// the metadata stage keeps one worker for as long as the test needs.
class HoldingProbeParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::Unknown; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".scanhold"}; }
    bool CanParse(const std::filesystem::path&) const override
    {
        if (!m_Held.exchange(true))
        {
            m_Entered.set_value();
            m_Gate.wait();
        }
        return true;
    }
    AssetParseResult Parse(const AssetMetadata&, AssetManager&) override { return AssetParseResult(false); }
    std::string GetName() const override { return "HoldingProbeParser"; }

    bool WaitUntilHeld(std::chrono::seconds timeout) const
    {
        return m_EnteredFuture.wait_for(timeout) == std::future_status::ready;
    }
    void Open()
    {
        if (!m_Opened.exchange(true))
            m_Open.set_value();
    }

  private:
    mutable std::atomic<bool> m_Held{false};
    mutable std::promise<void> m_Entered;
    std::future<void> m_EnteredFuture = m_Entered.get_future();
    std::atomic<bool> m_Opened{false};
    std::promise<void> m_Open;
    std::shared_future<void> m_Gate = m_Open.get_future().share();
};

void WriteFile(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << "probe";
}

} // namespace

// More than 256 files and two workers, so the metadata stage has no helpers and
// holds one worker. The registry stage waits in the dependency graph, so the
// other worker runs unrelated work while the metadata stage is held.
TEST(RegistryScan, WorkerIsNotParkedOnTheScanFuture)
{
    constexpr int kFiles = 300;
    constexpr std::chrono::seconds kTimeout{10};
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("ge_registry_scan_hold"));
    for (int i = 0; i < kFiles; ++i)
        WriteFile(root.Path() / ("probe_" + std::to_string(i) + ".scanhold"));

    auto parser = std::make_shared<HoldingProbeParser>();
    ParserRegistry parsers;
    ASSERT_TRUE(parsers.RegisterParser(parser));

    // Declared before the pool: the pool runs a queued task at its stop, so the task can outlive the check.
    std::promise<void> unrelatedRan;
    std::future<void> unrelatedDone = unrelatedRan.get_future();
    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry registry;
    ASSERT_TRUE(registry.Initialize(&pool));
    registry.SetParserRegistry(&parsers);

    // No early return until Open: a held stage stops the registry and the pool from stopping.
    std::future<size_t> scan = registry.ScanDirectoryAsync(root.Path(), true);
    const bool held = parser->WaitUntilHeld(kTimeout);
    EXPECT_TRUE(held) << "the metadata stage did not reach the parser";
    if (held)
    {
        pool.EnqueueWork([&unrelatedRan] { unrelatedRan.set_value(); });
        EXPECT_EQ(unrelatedDone.wait_for(kTimeout), std::future_status::ready)
            << "no worker ran unrelated work while the metadata stage was held: the registry stage holds the "
               "other worker";
    }
    parser->Open();

    ASSERT_EQ(scan.wait_for(std::chrono::seconds(60)), std::future_status::ready);
    EXPECT_NO_THROW((void)scan.get());

    registry.SetParserRegistry(nullptr);
    registry.Shutdown();
}

// More than 256 files, so the stage forks; the throwing file sits in a unit
// past the first, so a helper as well as the stage's own thread may take it.
TEST(RegistryScan, AChunkThatThrowsResolvesTheStagePromise)
{
    namespace fs = std::filesystem;
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("ge_registry_scan_throw"));
    constexpr int kFiles = 700;
    for (int i = 0; i < kFiles; ++i)
        WriteFile(root.Path() / ("probe_" + std::to_string(i) + ".scanprobe"));
    WriteFile(root.Path() / "throws_here.scanprobe");

    ParserRegistry parsers;
    ASSERT_TRUE(parsers.RegisterParser(std::make_shared<ThrowingProbeParser>()));

    JobSystem::WorkStealingThreadPool pool(4);
    AssetRegistry registry;
    ASSERT_TRUE(registry.Initialize(&pool));
    registry.SetParserRegistry(&parsers);

    std::future<size_t> scan = registry.ScanDirectoryAsync(root.Path(), true);
    ASSERT_EQ(scan.wait_for(std::chrono::seconds(60)), std::future_status::ready);
    try
    {
        (void)scan.get();
        ADD_FAILURE() << "the scan completed although a file's metadata extraction threw";
    }
    catch (const ScanProbeFailure& e)
    {
        EXPECT_STREQ(e.what(), "scan probe failed on throws_here.scanprobe");
    }
    catch (const std::exception& e)
    {
        ADD_FAILURE() << "the scan ended with \"" << e.what() << "\" in place of the unit's exception";
    }

    registry.SetParserRegistry(nullptr);
    registry.Shutdown();
}
