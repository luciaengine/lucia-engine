#pragma once

// kThreadSanitizerBuild is true when the test is compiled with ThreadSanitizer
// (clang's __has_feature, or GCC's __SANITIZE_THREAD__).

namespace GameEngine::TestBuild
{

#if defined(__SANITIZE_THREAD__)
inline constexpr bool kThreadSanitizerBuild = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
inline constexpr bool kThreadSanitizerBuild = true;
#else
inline constexpr bool kThreadSanitizerBuild = false;
#endif
#else
inline constexpr bool kThreadSanitizerBuild = false;
#endif

/// The skip text of a test that opens a media file through AVFoundation. AVFoundation creates the
/// shared NSURLSession on its first file load, and that loads a framework with dlopen. A race report
/// that forms on another thread at the same time deadlocks the ThreadSanitizer runtime: the report
/// waits for the dyld lock, and the dlopen thread waits for the runtime.
inline constexpr const char* kMediaLoadUnderThreadSanitizer =
    "ThreadSanitizer build only (#423): AVFoundation media loads deadlock the ThreadSanitizer runtime "
    "(a race report during the first NSURLSession dlopen); run this test in a build without ThreadSanitizer";

} // namespace GameEngine::TestBuild
