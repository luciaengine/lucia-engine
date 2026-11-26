// The focus policy of the process on macOS (Window::SetTakesFocus). A different process can activate the
// application, for example through the accessibility interface. A process that takes no focus must give
// that activation back.
#include "Platform/Window.h"

#include <gtest/gtest.h>

#import <Cocoa/Cocoa.h>

namespace
{
using GameEngine::Platform::Window;

// Its own process: the focus policy is process-wide and is set before Window::Initialize, as the Editor does.
TEST(WindowFocusPolicyMacTests, ApplicationThatTakesNoFocusHidesWhenItBecomesActive)
{
    @autoreleasepool {
        Window::SetTakesFocus(false);
        ASSERT_TRUE(Window::Initialize());
        ASSERT_FALSE([NSApp isHidden]);

        [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationDidBecomeActiveNotification
                                                            object:NSApp];
        // The hide request completes on the main run loop.
        const NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:2.0];
        while (![NSApp isHidden] && [deadline timeIntervalSinceNow] > 0.0)
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        EXPECT_TRUE([NSApp isHidden]);
    }
}

TEST(WindowFocusPolicyMacTests, RequestedEventWaitServicesTheMainRunLoop)
{
    @autoreleasepool {
        Window::SetTakesFocus(false);
        ASSERT_TRUE(Window::Initialize());
        Window::PollEvents();

        __block bool fired = false;
        CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(
            kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.005, 0.0, 0, 0,
            ^(CFRunLoopTimerRef) { fired = true; });
        ASSERT_NE(timer, nullptr);
        CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopDefaultMode);

        Window::WaitOnNextEventPoll(0.030);
        Window::PollEvents();

        EXPECT_TRUE(fired);
        CFRunLoopTimerInvalidate(timer);
        CFRelease(timer);
    }
}
}  // namespace
