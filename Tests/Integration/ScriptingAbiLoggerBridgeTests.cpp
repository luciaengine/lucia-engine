#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>

#include "Logger/FileSink.h"
#include "Logger/Logger.h"
#include "Scripting/ScriptingABI.h"
#include "Scripting/ScriptingTestHooks.h"
#include "TestTempDir.h"

using namespace std::filesystem;

#ifdef _DEBUG

// Only the _DEBUG tests below use these. Defining them outside the guard leaves them
// unreferenced in every other config, which is C4505 — an error wherever warnings are
// errors (DebugFast), so the whole target stops building.
namespace
{

#ifdef _WIN32
static void SetEnvGeLogFile(const std::string& path)
{
    _putenv_s("GE_LOGFILE", path.c_str());
}
#else
static void SetEnvGeLogFile(const std::string& path)
{
    setenv("GE_LOGFILE", path.c_str(), 1);
}
#endif

// Both tests leave a file sink attached and GE_LOGFILE pointing into this directory when
// they end, so it lives for the whole process.
static const path& LoggerBridgeTempDirectory()
{
    static const GameEngine::TestUtils::ScopedTempDir directory(
        GameEngine::TestUtils::MakeUniqueTempDirectory("GE_LoggerBridgeTests"));
    return directory.Path();
}

static std::string ReadAllText(const path& p)
{
    std::ifstream ifs(p, std::ios::in | std::ios::binary);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

} // namespace

TEST(ScriptingAbiLoggerBridge, AttachesFileSinkWhenNoSinksAndGeLogFileSet)
{
    using Logger::Log;

    // Reset bridge and logger state so we can exercise the lazy GE_LOGFILE wiring.
    GameEngine::ResetNativeLoggerBridgeForTests();

    // Ensure no sinks are present before the first GE_Log call.
    EXPECT_EQ(Log::GetSinkCount(), static_cast<size_t>(0));

    path tmpDir = LoggerBridgeTempDirectory();
    path logPath = tmpDir / "bridge_no_sinks.log";

    SetEnvGeLogFile(logPath.string());

    const char* msg = "[BridgeTest] Hello from GE_Log";
    ASSERT_EQ(GE_Log(GE_Log_Info, msg, static_cast<uint32_t>(strlen(msg))), GE_Result_Ok);
    Log::Flush();

    ASSERT_TRUE(exists(logPath));
    auto text = ReadAllText(logPath);
    EXPECT_NE(text.find("[BridgeTest] Hello from GE_Log"), std::string::npos);
}

TEST(ScriptingAbiLoggerBridge, RespectsPreconfiguredSinksAndIgnoresGeLogFile)
{
    using Logger::Log;

    GameEngine::ResetNativeLoggerBridgeForTests();

    // Manually configure a file sink before the first GE_Log call.
    path tmpDir = LoggerBridgeTempDirectory();
    path preconfiguredPath = tmpDir / "bridge_preconfigured.log";
    path ignoredPath = tmpDir / "bridge_ignored.log";

    Logger::Log::Config cfg;
    cfg.GlobalMinLevel = Logger::LogLevel::Debug;
    Log::Initialize(cfg);

    Logger::FileSink::Config fileCfg;
    fileCfg.filename = preconfiguredPath.string();
    fileCfg.append = false;
    fileCfg.minLevel = Logger::LogLevel::Debug;
    fileCfg.includeTimestamp = false;
    Log::AddSink(Logger::MakeUnique<Logger::FileSink>(fileCfg));

    EXPECT_GT(Log::GetSinkCount(), static_cast<size_t>(0));

    // Set GE_LOGFILE to a different path; the bridge should detect existing
    // sinks and *not* attach a new FileSink using this value.
    SetEnvGeLogFile(ignoredPath.string());

    const char* msg = "[BridgeTest] Preconfigured sink only";
    ASSERT_EQ(GE_Log(GE_Log_Info, msg, static_cast<uint32_t>(strlen(msg))), GE_Result_Ok);
    Log::Flush();

    ASSERT_TRUE(exists(preconfiguredPath));
    auto preText = ReadAllText(preconfiguredPath);
    EXPECT_NE(preText.find("Preconfigured sink only"), std::string::npos);

    // The ignored GE_LOGFILE path should not have been created or should be empty.
    if (exists(ignoredPath))
    {
        auto ignoredText = ReadAllText(ignoredPath);
        EXPECT_TRUE(ignoredText.empty());
    }
}

#else

TEST(ScriptingAbiLoggerBridge, SkippedInNonDebugBuilds)
{
    GTEST_SKIP() << "Native logger bridge tests rely on debug-only test hooks";
}

#endif

