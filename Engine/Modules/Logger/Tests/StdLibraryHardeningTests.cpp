// The standard library checks container preconditions exactly where GE_DEBUG_INSTRUMENTATION
// is 1 (root CMakeLists.txt). On libc++ a failed check in Debug goes through Logger's
// std::__libcpp_verbose_abort and aborts; in DebugFast it traps without a message.

#include <gtest/gtest.h>

#include "DeathTestChild.h"
#include "Logger/Logger.h"

#if defined(_WIN32)
#include <cstdlib>
#endif
#include <csignal>
#include <cstddef>
#include <vector>

namespace
{

#if defined(_LIBCPP_VERSION)
constexpr bool kStdLibraryChecksPreconditions = _LIBCPP_HARDENING_MODE != _LIBCPP_HARDENING_MODE_NONE;
#elif defined(_MSVC_STL_VERSION)
constexpr bool kStdLibraryChecksPreconditions = _MSVC_STL_HARDENING != 0 || _ITERATOR_DEBUG_LEVEL != 0;
#elif defined(_GLIBCXX_ASSERTIONS)
constexpr bool kStdLibraryChecksPreconditions = true;
#else
constexpr bool kStdLibraryChecksPreconditions = false;
#endif

TEST(StdLibraryHardening, ChecksPreconditionsExactlyInDevelopmentBuilds)
{
    EXPECT_EQ(kStdLibraryChecksPreconditions, GE_DEBUG_INSTRUMENTATION == 1);
}

#if defined(_LIBCPP_VERSION) && GE_DEBUG_INSTRUMENTATION
// Debug (the only configuration with _DEBUG) runs every libc++ check; DebugFast runs the fast ones.
TEST(StdLibraryHardening, LibcxxRunsEveryCheckInDebugAndTheFastChecksInDebugFast)
{
#if defined(_DEBUG)
    EXPECT_EQ(_LIBCPP_HARDENING_MODE, _LIBCPP_HARDENING_MODE_DEBUG);
#else
    EXPECT_EQ(_LIBCPP_HARDENING_MODE, _LIBCPP_HARDENING_MODE_FAST);
#endif
}
#endif

#if GE_DEBUG_INSTRUMENTATION
int ReadElement(const std::vector<int>& values, std::size_t index)
{
    return values[index];
}

// What the failed check prints before the process dies: Logger's Critical record in libc++ debug
// mode, the library's own line on libstdc++, nothing on the MSVC STL or in libc++ fast mode.
// The pattern does not match the handler's lowercase "[CRASH]" stderr line.
#if defined(_LIBCPP_VERSION) && _LIBCPP_HARDENING_MODE == _LIBCPP_HARDENING_MODE_DEBUG
constexpr const char* kFailedCheckReport = "Standard library check failed: .*Hardening assertion";
#elif defined(_LIBCPP_VERSION)
constexpr const char* kFailedCheckReport = "";
#elif defined(_GLIBCXX_ASSERTIONS)
constexpr const char* kFailedCheckReport = "Assertion .* failed";
#else
constexpr const char* kFailedCheckReport = "";
#endif

// The child's body: the default console sink writes Warning and above to stderr,
// which the death test reads.
void ReadPastTheEndOfAVector()
{
    GameEngine::TestUtils::SuppressCrtDialogsInDeathTestChild();
    Logger::Log::Initialize({});
    const std::vector<int> values(3);
    ReadElement(values, 5);
}

TEST(StdLibraryHardening, AnOutOfRangeSubscriptEndsTheProcessWithTheFailedCheck)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
#if defined(_MSVC_STL_VERSION)
    // abort() exits with code 3 and reaches the apps' SIGABRT handler; the STL's default
    // __fastfail would exit with 0xC0000409, so the exit code pins
    // _MSVC_STL_USE_ABORT_AS_DOOM_FUNCTION.
    EXPECT_EXIT(ReadPastTheEndOfAVector(), ::testing::ExitedWithCode(3), kFailedCheckReport);
#elif defined(_LIBCPP_VERSION) && _LIBCPP_HARDENING_MODE == _LIBCPP_HARDENING_MODE_FAST
    // Fast mode traps: brk (SIGTRAP) on arm64, ud2 (SIGILL) on x86-64. The apps' handler
    // catches SIGTRAP; this child has no handler, so the signal kills it.
#if defined(__aarch64__)
    EXPECT_EXIT(ReadPastTheEndOfAVector(), ::testing::KilledBySignal(SIGTRAP), kFailedCheckReport);
#else
    EXPECT_EXIT(ReadPastTheEndOfAVector(), ::testing::KilledBySignal(SIGILL), kFailedCheckReport);
#endif
#else
    EXPECT_DEATH(ReadPastTheEndOfAVector(), kFailedCheckReport);
#endif
}
#endif

} // namespace
