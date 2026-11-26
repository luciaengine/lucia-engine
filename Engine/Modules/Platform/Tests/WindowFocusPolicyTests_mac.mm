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

TEST(WindowFocusPolicyMacTests, RequestedEventWaitBoundsDispatchAndCompletesWindowUpdates)
{
    @autoreleasepool {
        Window::SetTakesFocus(false);
        ASSERT_TRUE(Window::Initialize());
        Window::PollEvents();

        constexpr NSInteger kTestEvent = 0x4745;
        constexpr int kQueuedEvents = 6;
        __block int dispatched = 0;
        __block bool updated = false;
        id monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskApplicationDefined
                                                          handler:^NSEvent*(NSEvent* event)
        {
            if (event.data1 == kTestEvent)
            {
                ++dispatched;
                [NSThread sleepForTimeInterval:0.004];
            }
            return event;
        }];
        NSNotificationCenter* notifications = [NSNotificationCenter defaultCenter];
        id observer = [notifications addObserverForName:NSApplicationDidUpdateNotification
                                                 object:NSApp
                                                  queue:nil
                                             usingBlock:^(NSNotification*) { updated |= dispatched > 0; }];
        // Application-defined test events stay inside this process.
        for (int index = 0; index < kQueuedEvents; ++index)
        {
            NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                              location:NSZeroPoint
                                         modifierFlags:0
                                             timestamp:0
                                          windowNumber:0
                                               context:nil
                                               subtype:0
                                                 data1:kTestEvent
                                                 data2:0];
            [NSApp postEvent:wake atStart:NO];
        }

        Window::WaitOnNextEventPoll(0.010);
        Window::PollEvents();

        EXPECT_GT(dispatched, 0);
        EXPECT_LT(dispatched, kQueuedEvents);
        EXPECT_TRUE(updated);
        [notifications removeObserver:observer];
        [NSEvent removeMonitor:monitor];
        Window::PollEvents();
    }
}
}  // namespace
