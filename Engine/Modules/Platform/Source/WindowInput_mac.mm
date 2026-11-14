#include "MacWindowInput.h"

extern "C" bool GE_MacOS_GetMouseButtonEventPosition(void* window, int button, double* x, double* y)
{
    return GameEngine::Platform::Detail::MouseButtonEventPosition(
        static_cast<NSWindow*>(window), NSApp.currentEvent, button, *x, *y);
}
