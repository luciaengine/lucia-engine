#include "MacWindowInput.h"
#include <gtest/gtest.h>

namespace
{
NSEvent* Click(NSWindow* window, NSPoint point, NSEventType type = NSEventTypeLeftMouseDown)
{
    return [NSEvent mouseEventWithType:type location:point modifierFlags:0
                           timestamp:0 windowNumber:window.windowNumber context:nil
                         eventNumber:1 clickCount:1 pressure:1];
}
}

TEST(MacWindowInput, UsesClickLocationRatherThanCurrentCursor)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        double x = -1, y = -1;
        EXPECT_TRUE(GameEngine::Platform::Detail::MouseButtonEventPosition(
            window, Click(window, NSMakePoint(123, 456)), 0, x, y));
        EXPECT_DOUBLE_EQ(x, 123);
        EXPECT_DOUBLE_EQ(y, 144);
        EXPECT_TRUE(GameEngine::Platform::Detail::MouseButtonEventPosition(
            window, Click(window, NSMakePoint(321, 234), NSEventTypeLeftMouseUp), 0, x, y));
        EXPECT_DOUBLE_EQ(x, 321);
        EXPECT_DOUBLE_EQ(y, 366);
        [window release];
    }
}

TEST(MacWindowInput, RejectsEventsForAnotherWindowOrButton)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        NSWindow* other = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 300)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        double x = -1, y = -1;
        EXPECT_FALSE(GameEngine::Platform::Detail::MouseButtonEventPosition(
            other, Click(window, NSMakePoint(123, 456)), 0, x, y));
        EXPECT_FALSE(GameEngine::Platform::Detail::MouseButtonEventPosition(
            window, Click(window, NSMakePoint(123, 456)), 1, x, y));
        EXPECT_FALSE(GameEngine::Platform::Detail::MouseButtonEventPosition(window, nil, 0, x, y));
        EXPECT_DOUBLE_EQ(x, -1);
        EXPECT_DOUBLE_EQ(y, -1);
        [other release];
        [window release];
    }
}
