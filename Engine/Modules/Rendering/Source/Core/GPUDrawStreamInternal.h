#pragma once

// Helpers that the GPUDrawStreamBuilder source files share. Not part of the module's interface.

#include "Rendering/Core/Device.h"

#include <cstddef>
#include <cstdint>

namespace GameEngine
{
namespace Rendering
{

// Mirrors VkDrawIndexedIndirectCommand. 5 uint32s = 20 bytes per record.
inline constexpr uint32_t kDrawCommandStride = 5 * sizeof(uint32_t);

// Winding parity rides bit 24 of the BatchTableEntry meshIndex field (real mesh indices are 24-bit:
// BatchRegistry::MakeKey masks them). A parity-1 (mirrored) sibling row sorts above all parity-0 rows
// of the same material. MUST match kMeshParityBit in Shaders/Includes/draw_stream_batch_table.glsl.
inline constexpr uint32_t kMeshParityBit = 1u << 24;

// A storage-buffer write of `bytes` from `offset`. A size of 0 binds the rest of the buffer.
inline DescriptorWrite StorageBufferWrite(uint32_t binding, BufferHandle buffer, size_t offset, size_t bytes)
{
    DescriptorWrite write{};
    write.Binding = binding;
    write.Type    = DescriptorType::StorageBuffer;
    write.Buffer  = buffer;
    write.Offset  = offset;
    write.Range   = bytes;
    return write;
}

} // namespace Rendering
} // namespace GameEngine
