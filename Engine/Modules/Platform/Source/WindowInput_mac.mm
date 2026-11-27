#include "MacWindowInput.h"

extern "C" bool GE_MacOS_GetMouseButtonEventPosition(void* window, int button, double* x, double* y)
{
    return GameEngine::Platform::Detail::MouseButtonEventPosition(
        static_cast<NSWindow*>(window), NSApp.currentEvent, button, *x, *y);
}

extern "C" void GE_MacOS_WaitForEventsAndUpdateWindows(double timeoutSeconds)
{
    @autoreleasepool
    {
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeoutSeconds];
        NSDate* waitUntil = deadline;
        do
        {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                              untilDate:waitUntil
                                                 inMode:NSDefaultRunLoopMode
                                                dequeue:YES];
            if (!event)
                break;
            [NSApp sendEvent:event];
            // Match the window-update step in AppKit's normal event loop.
            [NSApp updateWindows];
            waitUntil = [NSDate distantPast];
        } while ([deadline timeIntervalSinceNow] > 0.0);
    }
}
