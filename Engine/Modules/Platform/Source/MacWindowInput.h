#pragma once

#import <AppKit/AppKit.h>

namespace GameEngine::Platform::Detail
{
// Match GLFW's client coordinates: logical points, with the origin at the top left.
inline bool MouseButtonEventPosition(NSWindow* window, NSEvent* event, int button, double& x, double& y)
{
    if (!window || !event || event.window != window || event.buttonNumber != button)
        return false;
    if (event.type != NSEventTypeLeftMouseDown && event.type != NSEventTypeRightMouseDown &&
        event.type != NSEventTypeOtherMouseDown && event.type != NSEventTypeLeftMouseUp &&
        event.type != NSEventTypeRightMouseUp && event.type != NSEventTypeOtherMouseUp)
        return false;
    NSView* view = window.contentView;
    if (!view)
        return false;
    const NSPoint point = [view convertPoint:event.locationInWindow fromView:nil];
    x = point.x;
    y = view.isFlipped ? point.y : NSHeight(view.bounds) - point.y;
    return true;
}
}
