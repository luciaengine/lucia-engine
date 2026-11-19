#include "Rendering/Materials/Shader/ShaderLayoutShape.h"

namespace GameEngine::Rendering
{

void ApplyMetaImageShapeToLayout(const ShaderMeta& meta, DescriptorSetLayoutDesc& layout, uint32_t set)
{
    for (const auto& setMeta : meta.Sets)
    {
        if (setMeta.Set == set)
        {
            ApplySetMetaImageShapeToLayout(setMeta, layout);
            return;
        }
    }
}

void ApplySetMetaImageShapeToLayout(const DescriptorSetMeta& setMeta, DescriptorSetLayoutDesc& layout)
{
    for (auto& binding : layout.bindings)
    {
        for (const auto& bm : setMeta.Bindings)
        {
            if (bm.Binding != binding.binding)
                continue;

            // A `readonly buffer` is var<storage, read> in WGSL, and a
            // read_write layout entry is rejected against it.
            if (binding.type == DescriptorType::StorageBuffer)
            {
                if (bm.ReadOnly)
                    binding.flags |= kDescriptorBindingReadOnlyStorage;
                else
                    binding.flags &= ~kDescriptorBindingReadOnlyStorage;
            }

            if (!bm.Image.has_value())
                break;
            // Reflection keeps the historical convention — a cube reports Dim 2
            // with Cube set — while the layout's imageDim follows SPIR-V, where
            // 4 is cube. Without this a samplerCube binding declares a 2D view
            // and Dawn rejects the entry point against the layout.
            binding.imageDim = bm.Image->Cube ? 4u : bm.Image->Dim;
            binding.imageArrayed = bm.Image->Arrayed;
            // An unused shared GLSL sampler has no SPIR-V comparison operation to reflect.
            // The world pass still binds the engine's comparison sampler to this slot.
            binding.imageIsDepth = bm.Image->Depth ||
                (binding.type == DescriptorType::Sampler && bm.Name == "ge_shadowCompareSampler");
            binding.imageIsUnsignedInteger = bm.Image->UnsignedInteger;
            binding.imageMultisample = bm.Image->Multisample;
            binding.imageFilterableFloat = bm.Image->Filtered;
            binding.imageUsageReflected = true;
            if (binding.type == DescriptorType::StorageImage)
            {
                binding.storageTexelFormat = bm.Image->Format;
                binding.storageReadOnly = bm.ReadOnly;
            }
            break;
        }
    }
}

} // namespace GameEngine::Rendering
