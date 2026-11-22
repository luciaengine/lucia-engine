#include "AiAssistantSettings.h"
#include "Providers/BackendDetection.h"
#include "Providers/ClaudeApiProvider.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CodexSessionProvider.h"
#include "ScopedEnvironmentVariable.h"
#include "ScopedPreferencesRoot.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace GameEngine
{
namespace
{
#if defined(_WIN32)
constexpr const char* kSuffix = ".exe";
constexpr char kSeparator = ';';
#else
constexpr const char* kSuffix = "";
constexpr char kSeparator = ':';
#endif

// Writes an executable file named `program` in `folder`.
std::filesystem::path MakeProgram(const std::filesystem::path& folder, const std::string& program)
{
    std::filesystem::create_directories(folder);
    const std::filesystem::path file = folder / (program + kSuffix);
    std::ofstream(file) << "#!/bin/sh\n";
    std::filesystem::permissions(file, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
    return file;
}

// Detection searches only the test's folders for the scope of one test.
struct ScopedDetection
{
    explicit ScopedDetection(BackendDetection::Inputs inputs)
    {
        AiAssistantSettings::UseDetectionInputsForTesting(std::move(inputs));
    }
    ~ScopedDetection() { AiAssistantSettings::UseDetectionInputsForTesting(std::nullopt); }
};
} // namespace

TEST(BackendDetectionTests, AProgramOnPathIsFoundThereFirst)
{
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("BackendDetectionPath")};
    const std::filesystem::path onPath = MakeProgram(temp.Path() / "bin", "claude");
    MakeProgram(temp.Path() / "home" / ".local" / "bin", "claude");

    const auto found = BackendDetection::FindProgram(
        "claude", {.Path = "/nonexistent" + std::string(1, kSeparator) + (temp.Path() / "bin").string(),
                   .Home = temp.Path() / "home", .SystemFolders = false});

    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->Program, onPath);
    EXPECT_TRUE(found->OnPath);
}

// An editor opened from the desktop has a short PATH: the installer's folder still finds the program.
TEST(BackendDetectionTests, AProgramOffPathIsFoundInItsInstallFolder)
{
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("BackendDetectionInstall")};
    const std::filesystem::path installed = MakeProgram(temp.Path() / "home" / ".local" / "bin", "codex");

    const auto found = BackendDetection::FindProgram("codex", {.Path = "", .Home = temp.Path() / "home", .SystemFolders = false});

    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->Program, installed);
    EXPECT_FALSE(found->OnPath);
}

#if !defined(_WIN32)
TEST(BackendDetectionTests, AFileThatIsNotExecutableIsNotAProgram)
{
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("BackendDetectionNotExecutable")};
    std::filesystem::create_directories(temp.Path() / "bin");
    std::ofstream(temp.Path() / "bin" / "claude") << "text";

    EXPECT_FALSE(BackendDetection::FindProgram("claude", {.Path = (temp.Path() / "bin").string(), .Home = {}, .SystemFolders = false}));
}
#endif

// The executable setting, left empty, is the program's full path when only its install folder holds it.
TEST(BackendDetectionTests, AnEmptyExecutableIsTheDetectedPathOffPath)
{
    const ScopedPreferencesRoot preferences;
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("BackendDetectionSetting")};
    const std::filesystem::path installed = MakeProgram(temp.Path() / "home" / ".local" / "bin", "claude");
    const ScopedDetection detection({.Path = "", .Home = temp.Path() / "home", .SystemFolders = false});

    EXPECT_EQ(AiAssistantSettings::ClaudeExecutable(), installed.string());
    EXPECT_EQ(AiAssistantSettings::CodexExecutable(), std::string(CodexSessionProvider::kDefaultExecutable));
}

// Until the user chooses, the connection follows what is installed: claude, then codex, then a key.
TEST(BackendDetectionTests, TheFirstConnectionFollowsWhatIsInstalled)
{
    const ScopedPreferencesRoot preferences;
    const ScopedEnvironmentVariable noKey(std::string(ApiKeyStore::kVariable).c_str(), nullptr);
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("BackendDetectionProvider")};
    const std::filesystem::path home = temp.Path() / "home";
    const std::filesystem::path bin = temp.Path() / "bin";
    std::filesystem::create_directories(bin);
    // A home with no install folders, so only the test's PATH folder can hold a program.
    const BackendDetection::Inputs inputs{.Path = bin.string(), .Home = home, .SystemFolders = false};
    {
        const ScopedDetection detection(inputs);
        EXPECT_EQ(AiAssistantSettings::Provider(), std::string(ClaudeSessionProvider::kId)) << "nothing installed";
    }
    {
        const ScopedEnvironmentVariable key(std::string(ApiKeyStore::kVariable).c_str(), "fixture-key");
        const ScopedDetection detection(inputs);
        EXPECT_EQ(AiAssistantSettings::Provider(), std::string(ClaudeApiProvider::kId)) << "only a key";
    }
    MakeProgram(bin, "codex");
    {
        const ScopedDetection detection(inputs);
        EXPECT_EQ(AiAssistantSettings::Provider(), std::string(CodexSessionProvider::kId)) << "codex installed";
    }
    MakeProgram(bin, "claude");
    {
        const ScopedDetection detection(inputs);
        EXPECT_EQ(AiAssistantSettings::Provider(), std::string(ClaudeSessionProvider::kId)) << "both installed";
        AiAssistantSettings::SetProvider(std::string(CodexSessionProvider::kId));
        EXPECT_EQ(AiAssistantSettings::Provider(), std::string(CodexSessionProvider::kId)) << "a choice wins";
    }
}
#if defined(__APPLE__)
// DeepSeek Harness keeps dsh inside its app bundle, never on PATH.
TEST(BackendDetectionTests, HarnessIsFoundInsideItsAppBundle)
{
    const TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("BackendDetectionHarness")};
    const std::filesystem::path dsh = MakeProgram(
        temp.Path() / "home" / "Applications" / "Harness.app" / "Contents" / "Resources" / "runtime" / "cli" / "bin",
        "dsh");

    const auto found = BackendDetection::FindProgram("dsh", {.Path = "", .Home = temp.Path() / "home", .SystemFolders = false});

    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->Program, dsh);
    EXPECT_FALSE(found->OnPath);
}
#endif

} // namespace GameEngine
