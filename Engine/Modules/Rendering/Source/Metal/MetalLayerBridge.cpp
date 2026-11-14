#include "MetalLayerBridge.h"

#include <QuartzCore/QuartzCore.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <objc/message.h>
#include <objc/runtime.h>

#if defined(GE_PLATFORM_IOS)
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

namespace GameEngine::Rendering::MetalLayerBridge
{

namespace
{
using ObjcId = ::id;

#if !defined(GE_PLATFORM_IOS)
ObjcId Send(ObjcId receiver, const char* selector)
{
    using Fn = ObjcId (*)(ObjcId, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector));
}

#endif

void SendVoidBool(ObjcId receiver, const char* selector, bool value)
{
    using Fn = void (*)(ObjcId, SEL, BOOL);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value ? YES : NO);
}

#if !defined(GE_PLATFORM_IOS)
void SendVoidId(ObjcId receiver, const char* selector, ObjcId value)
{
    using Fn = void (*)(ObjcId, SEL, ObjcId);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value);
}

#endif

void SendVoidDouble(ObjcId receiver, const char* selector, double value)
{
    using Fn = void (*)(ObjcId, SEL, double);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value);
}

void SendVoidULong(ObjcId receiver, const char* selector, unsigned long value)
{
    using Fn = void (*)(ObjcId, SEL, unsigned long);
    reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector), value);
}

double SendDouble(ObjcId receiver, const char* selector)
{
    using Fn = double (*)(ObjcId, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(receiver, sel_registerName(selector));
}
} // namespace

#if defined(GE_PLATFORM_IOS)
CA::MetalLayer* CreateWindowLayer(void* window)
{
    auto* sdlWindow = static_cast<SDL_Window*>(window);
    const auto properties = SDL_GetWindowProperties(sdlWindow);
    auto view = SDL_Metal_CreateView(sdlWindow);
    if (view == nullptr) return nullptr;
    if (!SDL_SetPointerPropertyWithCleanup(properties, "GameEngine.MetalView", view,
        [](void*, void* value) { SDL_Metal_DestroyView(value); }, nullptr))
    {
        SDL_Metal_DestroyView(view);
        return nullptr;
    }
    auto* layer = static_cast<CA::MetalLayer*>(SDL_Metal_GetLayer(view));
    if (layer != nullptr) layer->retain();
    return layer;
}
#else
CA::MetalLayer* CreateWindowLayer(void* glfwWindow)
{
    if (glfwWindow == nullptr)
    {
        return nullptr;
    }

    auto* layer = CA::MetalLayer::layer();
    if (layer == nullptr) return nullptr;
    ObjcId nsWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(glfwWindow));
    if (nsWindow == nullptr)
    {
        return nullptr;
    }

    ObjcId contentView = Send(nsWindow, "contentView");
    if (contentView == nullptr)
    {
        return nullptr;
    }

    const double scale = SendDouble(nsWindow, "backingScaleFactor");

    ObjcId layerId = reinterpret_cast<ObjcId>(layer);
    SendVoidDouble(layerId, "setContentsScale:", scale > 0.0 ? scale : 1.0);
    SendVoidBool(contentView, "setWantsLayer:", true);
    SendVoidId(contentView, "setLayer:", layerId);
    layer->retain();
    return layer;
}

#endif

double GetWindowBackingScale(void* glfwWindow)
{
    if (glfwWindow == nullptr)
    {
        return 1.0;
    }
#if defined(GE_PLATFORM_IOS)
    return SDL_GetWindowDisplayScale(static_cast<SDL_Window*>(glfwWindow));
#else
    ObjcId nsWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(glfwWindow));
    if (nsWindow == nullptr)
    {
        return 1.0;
    }
    const double scale = SendDouble(nsWindow, "backingScaleFactor");
    return scale > 0.0 ? scale : 1.0;
#endif
}

void SetLayerDisplaySyncEnabled(CA::MetalLayer* layer, bool enabled)
{
#if defined(GE_PLATFORM_IOS)
    (void)layer;
    (void)enabled; // iOS presentation follows the display refresh.
#else
    if (layer != nullptr)
        SendVoidBool(reinterpret_cast<ObjcId>(layer), "setDisplaySyncEnabled:", enabled);
#endif
}

void SetLayerMaximumDrawableCount(CA::MetalLayer* layer, unsigned long count)
{
    if (layer != nullptr)
    {
        SendVoidULong(reinterpret_cast<ObjcId>(layer), "setMaximumDrawableCount:", count);
    }
}

bool SyncLayerContentsScale(void* glfwWindow, CA::MetalLayer* layer)
{
    if (glfwWindow == nullptr || layer == nullptr)
    {
        return false;
    }
    const double scale = GetWindowBackingScale(glfwWindow);
    ObjcId layerId = reinterpret_cast<ObjcId>(layer);
    const double currentScale = SendDouble(layerId, "contentsScale");
    if (scale <= 0.0 || currentScale == scale)
    {
        return false;
    }
    SendVoidDouble(layerId, "setContentsScale:", scale);
    return true;
}

void SetLayerExtendedDynamicRange(CA::MetalLayer* layer, bool enabled)
{
    if (layer == nullptr)
    {
        return;
    }
    ObjcId layerId = reinterpret_cast<ObjcId>(layer);
    SendVoidBool(layerId, "setWantsExtendedDynamicRangeContent:", enabled);
    CGColorSpaceRef colorSpace =
        enabled ? CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB) : nullptr;
    using SetColorSpaceFn = void (*)(ObjcId, SEL, CGColorSpaceRef);
    reinterpret_cast<SetColorSpaceFn>(objc_msgSend)(layerId, sel_registerName("setColorspace:"), colorSpace);
    if (colorSpace != nullptr)
    {
        CGColorSpaceRelease(colorSpace); // the layer retains it
    }
}

void GetWindowEdrHeadroom(void* glfwWindow, float& outCurrent, float& outPotential)
{
    outCurrent = 1.0f;
    outPotential = 1.0f;
#if defined(GE_PLATFORM_IOS)
    (void)glfwWindow;
#else
    if (glfwWindow == nullptr)
    {
        return;
    }
    ObjcId nsWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(glfwWindow));
    if (nsWindow == nullptr)
    {
        return;
    }
    ObjcId screen = Send(nsWindow, "screen");
    if (screen == nullptr)
    {
        return;
    }
    const double current = SendDouble(screen, "maximumExtendedDynamicRangeColorComponentValue");
    const double potential = SendDouble(screen, "maximumPotentialExtendedDynamicRangeColorComponentValue");
    outCurrent = current > 0.0 ? static_cast<float>(current) : 1.0f;
    outPotential = potential > 0.0 ? static_cast<float>(potential) : 1.0f;
#endif
}

bool GetWindowPixelSize(void* window, CA::MetalLayer* layer, unsigned int& outWidth, unsigned int& outHeight)
{
    outWidth = 0;
    outHeight = 0;
    if (layer == nullptr)
    {
        return false;
    }
#if defined(GE_PLATFORM_IOS)
    int width = 0, height = 0;
    if (!SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(window), &width, &height) || width <= 0 || height <= 0)
        return false;
    outWidth = static_cast<unsigned int>(width);
    outHeight = static_cast<unsigned int>(height);
    return true;
#else
    (void)window;
    ObjcId layerId = reinterpret_cast<ObjcId>(layer);
    using BoundsFn = CGRect (*)(ObjcId, SEL);
#if defined(__aarch64__)
    const CGRect bounds = reinterpret_cast<BoundsFn>(objc_msgSend)(layerId, sel_registerName("bounds"));
#else
    const CGRect bounds = reinterpret_cast<BoundsFn>(objc_msgSend_stret)(layerId, sel_registerName("bounds"));
#endif
    const double scale = SendDouble(layerId, "contentsScale");
    const double effectiveScale = scale > 0.0 ? scale : 1.0;
    const double w = bounds.size.width * effectiveScale;
    const double h = bounds.size.height * effectiveScale;
    if (w < 1.0 || h < 1.0)
    {
        return false;
    }
    outWidth = static_cast<unsigned int>(w + 0.5);
    outHeight = static_cast<unsigned int>(h + 0.5);
    return true;
#endif
}

} // namespace GameEngine::Rendering::MetalLayerBridge
