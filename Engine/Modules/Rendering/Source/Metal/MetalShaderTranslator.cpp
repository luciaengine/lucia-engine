#include "MetalShaderTranslator.h"

#include "MetalArgumentBufferLayout.h"
#include "MetalDirectBindings.h"
#include "Rendering/Core/SpecializationConstants.h"

#include "Logger/Logger.h"
#include "Types/Fnv1a.h"
#include "Platform/Environment.h"

#include <spirv_cross/spirv_msl.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace GameEngine::Rendering
{

namespace
{

// On-disk cache for translated MSL. SPIRV-Cross translation is pure CPU work
// repeated identically every launch (~1.3s of the first scene frame in the
// editor); macOS's automatic shader cache only covers the Metal compiler
// stages after it. Keyed by a content hash of every translation input.
// GE_METAL_NO_MSL_CACHE=1 disables it when bisecting translator changes.
constexpr uint32_t kMslCacheVersion = 3; // bump when translator options change

// Seed of the MSL cache key. It is not the FNV-1a offset basis (one digit
// short), but the key names cache files on disk: changing it orphans every
// cached translation.
constexpr uint64_t kMslCacheKeySeed = 1469598103934665603ull;

uint64_t ComputeTranslationHash(const std::vector<uint8_t>& spirv, MetalShaderStage stage,
                                const std::vector<const DescriptorSetLayoutDesc*>& setLayouts,
                                const SpecializationConstants* specialization, bool tier2ArgumentBuffers)
{
    uint64_t hash = Hashing::Fnv1a64(&kMslCacheVersion, sizeof(kMslCacheVersion), 1469598103934665603ull);
    hash = Hashing::Fnv1a64(&tier2ArgumentBuffers, sizeof(tier2ArgumentBuffers), hash);
#if defined(GE_PLATFORM_IOS)
    constexpr uint32_t platform = 1;
#else
    constexpr uint32_t platform = 0;
#endif
    hash = Hashing::Fnv1a64(&platform, sizeof(platform), hash);
    hash = Hashing::Fnv1a64(spirv.data(), spirv.size(), hash);
    const uint32_t stageValue = static_cast<uint32_t>(stage);
    hash = Hashing::Fnv1a64(&stageValue, sizeof(stageValue), hash);
    for (const DescriptorSetLayoutDesc* layout : setLayouts)
    {
        const uint32_t marker = layout != nullptr ? 1u : 0u;
        hash = Hashing::Fnv1a64(&marker, sizeof(marker), hash);
        if (layout == nullptr)
        {
            continue;
        }
        for (const DescriptorBinding& binding : layout->bindings)
        {
            const uint32_t fields[5] = {binding.binding, static_cast<uint32_t>(binding.type), binding.count,
                                        binding.shaderStages, binding.flags};
            hash = Hashing::Fnv1a64(fields, sizeof(fields), hash);
        }
    }
    if (specialization != nullptr && !specialization->IsEmpty())
    {
        const uint8_t* data = specialization->GetDataPtr();
        for (const auto& [id, entry] : specialization->GetEntries())
        {
            hash = Hashing::Fnv1a64(&id, sizeof(id), hash);
            hash = Hashing::Fnv1a64(data + entry.Offset, entry.Size, hash);
        }
    }
    return hash;
}

bool MslCacheEnabled()
{
    static const bool kDisabled = Platform::EnvironmentSwitchEnabled("GE_METAL_NO_MSL_CACHE", false);
    return !kDisabled;
}

std::filesystem::path MslCachePath(uint64_t hash)
{
    static const std::filesystem::path kDir = []() {
        const char* home = std::getenv("HOME");
        std::filesystem::path dir = home != nullptr
                                        ? std::filesystem::path(home) / "Library/Caches/GameEngine/MetalMSL"
                                        : std::filesystem::path("/tmp/GameEngineMetalMSL");
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }();
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.metal", static_cast<unsigned long long>(hash));
    return kDir / name;
}

bool TryLoadCachedMsl(const std::filesystem::path& path, MetalShaderTranslation& outResult)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }
    std::string header;
    std::getline(file, header);
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    if (std::sscanf(header.c_str(), "// GE-MSL local %u %u %u", &x, &y, &z) != 3)
    {
        return false;
    }
    std::ostringstream body;
    body << file.rdbuf();
    if (body.str().empty())
    {
        return false;
    }
    outResult.Msl = body.str();
    outResult.EntryPoint = "main0";
    outResult.LocalSizeX = x;
    outResult.LocalSizeY = y;
    outResult.LocalSizeZ = z;
    outResult.Success = true;
    return true;
}

void StoreCachedMsl(const std::filesystem::path& path, const MetalShaderTranslation& result)
{
    // Unique temp + rename keeps concurrent prewarm threads from ever
    // exposing a partially written file.
    std::filesystem::path tmp = path;
    tmp += "." + std::to_string(reinterpret_cast<uintptr_t>(&path));
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            return;
        }
        file << "// GE-MSL local " << result.LocalSizeX << ' ' << result.LocalSizeY << ' ' << result.LocalSizeZ
             << '\n';
        file << result.Msl;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        std::filesystem::remove(tmp, ec);
    }
}

spv::ExecutionModel ToExecutionModel(MetalShaderStage stage)
{
    switch (stage)
    {
    case MetalShaderStage::Vertex:   return spv::ExecutionModelVertex;
    case MetalShaderStage::Fragment: return spv::ExecutionModelFragment;
    case MetalShaderStage::Compute:  return spv::ExecutionModelGLCompute;
    case MetalShaderStage::Mesh:     return spv::ExecutionModelMeshEXT;
    case MetalShaderStage::Object:   return spv::ExecutionModelTaskEXT;
    }
    return spv::ExecutionModelVertex;
}

void AddLayoutBindings(spirv_cross::CompilerMSL& compiler, spv::ExecutionModel model,
                       const std::vector<const DescriptorSetLayoutDesc*>& setLayouts, bool tier2ArgumentBuffers)
{
    std::vector<MetalArgumentBufferLayout> layouts;
    for (const auto* layout : setLayouts)
        layouts.push_back(layout ? ComputeMetalArgumentBufferLayout(*layout) : MetalArgumentBufferLayout{});
    if (!tier2ArgumentBuffers) layouts = ComputeMetalDirectBindings(layouts);
    for (uint32_t set = 0; set < setLayouts.size(); ++set)
    {
        if (setLayouts[set] == nullptr)
        {
            continue;
        }
        const MetalArgumentBufferLayout& layout = layouts[set];
        for (const MetalArgumentSlot& slot : layout.Slots)
        {
            spirv_cross::MSLResourceBinding binding{};
            binding.stage = model;
            binding.desc_set = set;
            binding.binding = slot.Binding;
            binding.count = slot.Count;
            // pad_argument_buffer_resources requires a base type so the
            // compiler knows which argument-table id(s) the binding occupies.
            switch (slot.Type)
            {
            case DescriptorType::UniformBuffer:
            case DescriptorType::StorageBuffer:
                binding.basetype = spirv_cross::SPIRType::Void;
                break;
            case DescriptorType::Texture:
            case DescriptorType::StorageImage:
                binding.basetype = spirv_cross::SPIRType::Image;
                break;
            case DescriptorType::Sampler:
                binding.basetype = spirv_cross::SPIRType::Sampler;
                break;
            case DescriptorType::CombinedImageSampler:
                binding.basetype = spirv_cross::SPIRType::SampledImage;
                break;
            case DescriptorType::AccelerationStructure:
                // Void, not SPIRType::AccelerationStructure: the padding pass
                // rejects that base type outright ("Unexpected argument buffer
                // resource base type"), and get_metal_resource_index routes
                // everything that is not an image or a sampler through
                // msl_buffer regardless. The emitted member is still a real
                // raytracing::acceleration_structure at the requested id.
                binding.basetype = spirv_cross::SPIRType::Void;
                break;
            }
            if (slot.BufferId != MetalArgumentSlot::kUnused)
            {
                binding.msl_buffer = slot.BufferId;
            }
            if (slot.AccelerationStructureId != MetalArgumentSlot::kUnused)
            {
                binding.msl_buffer = slot.AccelerationStructureId;
            }
            if (slot.TextureId != MetalArgumentSlot::kUnused)
            {
                binding.msl_texture = slot.TextureId;
            }
            if (slot.SamplerId != MetalArgumentSlot::kUnused)
            {
                binding.msl_sampler = slot.SamplerId;
            }
            compiler.add_msl_resource_binding(binding);
        }

        // Shaders using runtime-array .length() make SPIRV-Cross emit a
        // buffer-size-constants member inside the set's argument buffer.
        // Without an explicit id it lands on id(0) and overlaps the first
        // resource (emitted as an invalid reinterpret_cast). Reserve the slot
        // right after the layout's resources; CreateDescriptorSet stores there
        // the address of the sizes it keeps after the set's table.
        if (!tier2ArgumentBuffers) continue;
        spirv_cross::MSLResourceBinding sizeBinding{};
        sizeBinding.stage = model;
        sizeBinding.basetype = spirv_cross::SPIRType::Void;
        sizeBinding.desc_set = set;
        sizeBinding.binding = spirv_cross::kBufferSizeBufferBinding;
        sizeBinding.count = 1;
        sizeBinding.msl_buffer = layout.SizeConstantsSlot;
        compiler.add_msl_resource_binding(sizeBinding);
    }

    // Push constants -> reserved buffer index outside the argument buffer range.
    spirv_cross::MSLResourceBinding pushConstants{};
    pushConstants.stage = model;
    pushConstants.basetype = spirv_cross::SPIRType::Void;
    pushConstants.desc_set = spirv_cross::kPushConstDescSet;
    pushConstants.binding = spirv_cross::kPushConstBinding;
    pushConstants.count = 1;
    pushConstants.msl_buffer = kMetalPushConstantBufferIndex;
    compiler.add_msl_resource_binding(pushConstants);
}

// Decorate gl_Position as invariant so separately compiled pipelines (depth
// prepass vs forward color) rasterize bit-identical depth — the reverse-Z
// GreaterOrEqual prepass contract depends on it, and Metal only guarantees
// cross-pipeline position equality for [[position, invariant]] outputs.
void ForceInvariantPosition(spirv_cross::CompilerMSL& compiler)
{
    for (spirv_cross::VariableID variableId : compiler.get_active_interface_variables())
    {
        // Plain `out vec4 gl_Position`-style output.
        if (compiler.has_decoration(variableId, spv::DecorationBuiltIn) &&
            compiler.get_decoration(variableId, spv::DecorationBuiltIn) == spv::BuiltInPosition)
        {
            compiler.set_decoration(variableId, spv::DecorationInvariant);
            continue;
        }
        // gl_PerVertex block: position is a struct member.
        const spirv_cross::SPIRType& type = compiler.get_type_from_variable(variableId);
        if (type.basetype != spirv_cross::SPIRType::Struct)
        {
            continue;
        }
        for (uint32_t member = 0; member < type.member_types.size(); ++member)
        {
            if (compiler.has_member_decoration(type.self, member, spv::DecorationBuiltIn) &&
                compiler.get_member_decoration(type.self, member, spv::DecorationBuiltIn) == spv::BuiltInPosition)
            {
                compiler.set_member_decoration(type.self, member, spv::DecorationInvariant);
            }
        }
    }
}

// Bake engine specialization-constant values into the module so the MSL
// function constants default to the requested values. Each unique value set
// produces distinct MSL, which the pipeline cache already keys by content.
void ApplySpecializationConstants(spirv_cross::CompilerMSL& compiler, const SpecializationConstants* specialization)
{
    if (specialization == nullptr || specialization->IsEmpty())
    {
        return;
    }
    const uint8_t* data = specialization->GetDataPtr();
    auto spirvConstants = compiler.get_specialization_constants();
    for (const spirv_cross::SpecializationConstant& sc : spirvConstants)
    {
        for (const auto& [id, entry] : specialization->GetEntries())
        {
            if (id != sc.constant_id || entry.Size > 8)
            {
                continue;
            }
            auto& constant = compiler.get_constant(sc.id);
            uint64_t raw = 0;
            std::memcpy(&raw, data + entry.Offset, entry.Size);
            if (entry.Size == 8)
            {
                constant.m.c[0].r[0].u64 = raw;
            }
            else
            {
                constant.m.c[0].r[0].u32 = static_cast<uint32_t>(raw);
            }
        }
    }
}

} // namespace

MetalShaderTranslation TranslateSpirvToMsl(const std::vector<uint8_t>& spirv,
                                           MetalShaderStage stage,
                                           const std::vector<const DescriptorSetLayoutDesc*>& setLayouts,
                                           const SpecializationConstants* specialization, bool tier2ArgumentBuffers)
{
    MetalShaderTranslation result{};
    if (spirv.size() < 4 || (spirv.size() % 4) != 0)
    {
        result.Error = "SPIR-V blob has invalid size";
        return result;
    }

    std::filesystem::path cachePath;
    if (MslCacheEnabled())
    {
        cachePath = MslCachePath(ComputeTranslationHash(spirv, stage, setLayouts, specialization, tier2ArgumentBuffers));
        if (TryLoadCachedMsl(cachePath, result))
        {
            return result;
        }
    }

    try
    {
        std::vector<uint32_t> words(spirv.size() / 4);
        std::memcpy(words.data(), spirv.data(), spirv.size());

        spirv_cross::CompilerMSL compiler(std::move(words));

        const spv::ExecutionModel model = ToExecutionModel(stage);
        const std::string spirvEntryName(compiler.get_entry_points_and_stages()[0].name);
        compiler.set_entry_point(spirvEntryName, model);

        spirv_cross::CompilerMSL::Options options{};
#if defined(GE_PLATFORM_IOS)
        options.platform = spirv_cross::CompilerMSL::Options::iOS;
        options.set_msl_version(tier2ArgumentBuffers ? 3 : 2, tier2ArgumentBuffers ? 0 : 3);
#else
        options.platform = spirv_cross::CompilerMSL::Options::macOS;
        options.set_msl_version(3, 0);
#endif
        options.argument_buffers = tier2ArgumentBuffers;
        options.buffer_size_buffer_index = kMetalDirectBufferSizesIndex;
        options.argument_buffers_tier = tier2ArgumentBuffers
            ? spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2
            : spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier1;
        options.force_active_argument_buffer_resources = tier2ArgumentBuffers;
        // Positional padding is load-bearing: the GPU reads the argument
        // buffer through the generated struct, so member POSITION is the
        // memory offset. Padding keeps position == [[id(n)]] for bindings a
        // stage doesn't use, matching the contiguous slot ids assigned in
        // ComputeMetalArgumentBufferLayout.
        options.pad_argument_buffer_resources = tier2ArgumentBuffers;
        options.pad_fragment_output_components = true;
        options.texture_buffer_native = true;
        // The swizzle-buffer default (30) collides with the engine's push
        // constant index; swizzling is unused but keep the index clear.
        options.swizzle_buffer_index = 26;
        compiler.set_msl_options(options);

        AddLayoutBindings(compiler, model, setLayouts, tier2ArgumentBuffers);
        ApplySpecializationConstants(compiler, specialization);
        if (stage == MetalShaderStage::Vertex)
        {
            ForceInvariantPosition(compiler);
        }
        if (stage == MetalShaderStage::Mesh || stage == MetalShaderStage::Object)
        {
            // glslang emits the mesh workgroup size as LocalSizeId (constant-id
            // form). SPIRV-Cross emits gl_WorkGroupSize = uint3(0,0,0) for that
            // form, and the generated vertex/primitive emit loops advance by
            // `gl_WorkGroupSize.x*y*z == 0` — an infinite loop that hangs the GPU.
            // Resolve the id constants to literals and re-state them as a plain
            // LocalSize so gl_WorkGroupSize is correct.
            uint32_t wx = 1, wy = 1, wz = 1;
            const spirv_cross::Bitset& modes = compiler.get_execution_mode_bitset();
            if (modes.get(spv::ExecutionModeLocalSizeId))
            {
                wx = compiler.evaluate_constant_u32(
                    compiler.get_execution_mode_argument(spv::ExecutionModeLocalSizeId, 0));
                wy = compiler.evaluate_constant_u32(
                    compiler.get_execution_mode_argument(spv::ExecutionModeLocalSizeId, 1));
                wz = compiler.evaluate_constant_u32(
                    compiler.get_execution_mode_argument(spv::ExecutionModeLocalSizeId, 2));
                compiler.unset_execution_mode(spv::ExecutionModeLocalSizeId);
            }
            else if (modes.get(spv::ExecutionModeLocalSize))
            {
                wx = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0);
                wy = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1);
                wz = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2);
            }
            compiler.set_execution_mode(spv::ExecutionModeLocalSize, wx == 0 ? 1 : wx,
                                        wy == 0 ? 1 : wy, wz == 0 ? 1 : wz);
        }

        result.Msl = compiler.compile();
        result.EntryPoint = "main0";

        if (stage == MetalShaderStage::Compute || stage == MetalShaderStage::Mesh ||
            stage == MetalShaderStage::Object)
        {
            // The MSL output renames the entry to main0, but the internal
            // entry-point registry keeps the SPIR-V name. Mesh/object stages
            // carry a local workgroup size too (threads per mesh threadgroup).
            const auto& entry = compiler.get_entry_point(spirvEntryName, model);
            result.LocalSizeX = entry.workgroup_size.x;
            result.LocalSizeY = entry.workgroup_size.y;
            result.LocalSizeZ = entry.workgroup_size.z;
        }
        result.Success = true;
    }
    catch (const std::exception& e)
    {
        result.Error = e.what();
    }
    if (result.Success && !cachePath.empty())
    {
        StoreCachedMsl(cachePath, result);
    }
    return result;
}

} // namespace GameEngine::Rendering
