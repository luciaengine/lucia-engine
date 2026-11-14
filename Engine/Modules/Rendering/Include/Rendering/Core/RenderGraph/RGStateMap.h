#pragma once

// RenderGraph barrier-IR layout ↔ engine ResourceState mapping. Used for (a) seeding
// imported resources at the pool's tracked state and (b) the end-of-frame
// write-back. Pool-backed resources form a CLOSED LOOP through these two
// functions, so every state the pool can hold round-trips exactly; the lossy
// entries (Common) only occur for externally-provided states, where the caller
// supplies the truth explicitly.

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"

namespace GameEngine::Rendering::RenderGraph
{

/// Converts graph access scopes to the engine barrier mask used by command recording.
inline uint64_t ToEngineAccessMask(uint32_t accessMask)
{
    uint64_t engineMask = 0;
    if (accessMask & RGAccessMask::IndirectRead)
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::IndirectCommandRead);
    if (accessMask & RGAccessMask::IndexRead)
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::IndexRead);
    if (accessMask & RGAccessMask::VertexAttribRead)
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::VertexAttributeRead);
    if (accessMask & (RGAccessMask::UniformRead | RGAccessMask::ShaderRead))
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::ShaderRead);
    if (accessMask & RGAccessMask::ShaderWrite)
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::ShaderWrite);
    if (accessMask & RGAccessMask::ColorWrite)
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::ColorAttachmentWrite);
    if (accessMask & RGAccessMask::ColorRead)
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::ColorAttachmentRead);
    if (accessMask & RGAccessMask::DepthRead)
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::DepthStencilRead);
    if (accessMask & RGAccessMask::DepthWrite)
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::DepthStencilWrite);
    if (accessMask & RGAccessMask::TransferRead)
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::TransferRead);
    if (accessMask & RGAccessMask::TransferWrite)
        engineMask |= static_cast<uint64_t>(ResourceAccessMask::TransferWrite);
    return engineMask;
}

inline ResourceState ToResourceState(RGImageLayout l)
{
    switch (l)
    {
        case RGImageLayout::Undefined: return ResourceState::Undefined;
        case RGImageLayout::General: return ResourceState::UnorderedAccess;
        case RGImageLayout::ColorAttachment: return ResourceState::RenderTarget;
        case RGImageLayout::DepthAttachment: return ResourceState::DepthWrite;
        case RGImageLayout::DepthReadOnly: return ResourceState::DepthRead;
        case RGImageLayout::ShaderReadOnly: return ResourceState::ShaderResource;
        case RGImageLayout::TransferSrc: return ResourceState::CopySource;
        case RGImageLayout::TransferDst: return ResourceState::CopyDest;
        case RGImageLayout::Present: return ResourceState::Common;
    }
    return ResourceState::Undefined;
}

inline RGImageLayout ToImageLayout(ResourceState s)
{
    switch (s)
    {
        case ResourceState::ShaderResource: return RGImageLayout::ShaderReadOnly;
        case ResourceState::RenderTarget: return RGImageLayout::ColorAttachment;
        case ResourceState::DepthWrite: return RGImageLayout::DepthAttachment;
        case ResourceState::DepthRead: return RGImageLayout::DepthReadOnly;
        case ResourceState::DepthSampled: return RGImageLayout::DepthReadOnly;
        case ResourceState::UnorderedAccess: return RGImageLayout::General;
        case ResourceState::CopySource: return RGImageLayout::TransferSrc;
        case ResourceState::CopyDest: return RGImageLayout::TransferDst;
        default:
            // Unknown/Common/buffer-ish states: Undefined forces a discarding
            // transition — only correct because the pool loop never stores
            // these for textures whose content must survive.
            return RGImageLayout::Undefined;
    }
}

} // namespace GameEngine::Rendering::RenderGraph
