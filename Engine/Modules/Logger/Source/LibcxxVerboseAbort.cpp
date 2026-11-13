// libc++ calls this hook when a standard-library check fails in Debug (debug hardening mode).
// The hook logs the message at Critical, writes it to stderr, flushes the log, and aborts.
// The abort runs the apps' SIGABRT handler, as a failed assert() does.

#include <version>

#if defined(_LIBCPP_VERSION) && GE_DEBUG_INSTRUMENTATION

#include "Logger/Logger.h"

#include <__verbose_abort>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace
{

// Longer than any libc++ check message, which is a header path, a line, the
// checked expression and a short reason.
constexpr size_t kMessageCapacity = 1024;

// Set by the first failed check. A check that fails while it is being reported
// (inside a sink, or on a second thread) aborts without logging.
std::atomic<bool> s_Reporting{false};

} // namespace

[[noreturn]] void std::__libcpp_verbose_abort(const char* format, ...) noexcept
{
    if (s_Reporting.exchange(true))
        std::abort();

    char message[kMessageCapacity];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    std::string_view text = message;
    while (!text.empty() && text.back() == '\n')
        text.remove_suffix(1);

    // Symbolizing a backtrace here can fault if the check failed during process
    // teardown; the SIGABRT handler and the OS crash report carry the stack.
    Logger::Log::SuppressBacktraceCapture();
    Logger::Log::Critical("Standard library check failed: {}", text);
    // Reaches stderr even when no sink does. With a stderr console sink the failure
    // prints twice, as the apps' crash handlers' reports do.
    std::fprintf(stderr, "[CRASH] standard library check failed: %.*s\n",
                 static_cast<int>(text.size()), text.data());
    std::fflush(stderr);
    Logger::Log::FlushForCrash(Logger::Log::kCrashFlushTimeout);
    std::abort();
}

#endif
