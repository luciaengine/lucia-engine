#pragma once

#include <functional>

namespace MTL
{
class CommandBuffer;
}

namespace GameEngine::Rendering
{

/// Adds a handler that Metal calls on its own thread when the GPU completes the buffer, or when Metal
/// destroys the buffer uncommitted. Use it for every completion handler of the Metal backend: the order
/// from this call to the handler goes through the kernel, and the function tells the thread sanitizer
/// about that order, so the handler can read what the caller wrote before the call.
void AddCompletedHandler(MTL::CommandBuffer* buffer, std::function<void(MTL::CommandBuffer*)> handler);

} // namespace GameEngine::Rendering
