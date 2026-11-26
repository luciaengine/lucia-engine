#pragma once

#import <AppKit/AppKit.h>

#include <optional>

namespace GameEngine::Platform::Detail
{
// AppKit's setting is process-wide, so restore the previous value when the drag ends.
class ScopedUncoalescedMouseInput
{
public:
    ScopedUncoalescedMouseInput() = default;
    ~ScopedUncoalescedMouseInput() { End(); }
    ScopedUncoalescedMouseInput(const ScopedUncoalescedMouseInput&) = delete;
    ScopedUncoalescedMouseInput& operator=(const ScopedUncoalescedMouseInput&) = delete;

    void Begin()
    {
        if (m_Previous.has_value())
            return;
        m_Previous = NSEvent.mouseCoalescingEnabled;
        NSEvent.mouseCoalescingEnabled = NO;
    }

    void End()
    {
        if (!m_Previous.has_value())
            return;
        NSEvent.mouseCoalescingEnabled = *m_Previous;
        m_Previous.reset();
    }

private:
    std::optional<BOOL> m_Previous;
};

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
