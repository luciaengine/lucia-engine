#include "MetalCompletedHandler.h"

#include <Metal/Metal.hpp>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define GE_METAL_TSAN 1
#include <sanitizer/tsan_interface.h>
#endif
#endif

namespace GameEngine::Rendering
{

void AddCompletedHandler(MTL::CommandBuffer* buffer, std::function<void(MTL::CommandBuffer*)> handler)
{
    // A raw block, not the std::function overload of metal-cpp: that overload reads its copy of the function
    // before any code here runs. The acquire is the first statement and uses only the parameter, because the
    // captures live in the block copy that Metal makes on this thread.
    buffer->addCompletedHandler(^(MTL::CommandBuffer* completed) {
#if defined(GE_METAL_TSAN)
        __tsan_acquire(completed);
#endif
        handler(completed);
    });
#if defined(GE_METAL_TSAN)
    // After addCompletedHandler, so the release also covers the block copy.
    __tsan_release(buffer);
#endif
}

} // namespace GameEngine::Rendering
