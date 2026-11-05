// Tests for the MaterialBinder service. Exercises the name-driven set
// resolution against three sources (DrawBindings, PassResources, Material),
// the bindless texture short-circuit, and the BeginPass / EndPass /
// OnBeginFrame lifecycle against a real Vulkan device — no production callers
// are wired yet, so we validate via direct API calls.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "AssetCore/GUID.h"
#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/PassBindingContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Types/Color.h"
#include "Types/StringId.h"

#include "TestDeviceHelper.h"
#include "TestTempDir.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

namespace
{

// Helper: allocate a host-visible buffer with the given usage. Tests just
// need a valid handle for descriptor writes — no draw is actually issued.
BufferHandle MakeTestBuffer(IDevice& dev, size_t size, BufferUsage usage)
{
    BufferDesc desc{};
    desc.size        = size;
    desc.usage       = static_cast<uint32_t>(usage);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName   = "MaterialBinderTests.Buffer";
    return dev.CreateBuffer(desc);
}

// Build a synthetic ShaderMeta declaring set 2 with two SSBO bindings —
// mirrors terrain's PatchSSBO + ParamsSSBO shape.
std::shared_ptr<ShaderMeta> MakeTerrainStyleMeta()
{
    auto meta = std::make_shared<ShaderMeta>();
    DescriptorSetMeta s{};
    s.Set = 2;

    DescriptorBindingMeta b0{};
    b0.Binding = 0;
    b0.Name = "PatchSSBO";
    b0.Type = ShaderMetaBindingType::kStorageBuffer;
    b0.Count = 1;
    b0.StagesMask = (1u << 0) | (1u << 1);
    s.Bindings.push_back(b0);

    DescriptorBindingMeta b1{};
    b1.Binding = 1;
    b1.Name = "ParamsSSBO";
    b1.Type = ShaderMetaBindingType::kStorageBuffer;
    b1.Count = 1;
    b1.StagesMask = (1u << 0) | (1u << 1);
    s.Bindings.push_back(b1);

    meta->Sets.push_back(s);
    return meta;
}

// Synthesize an empty PassBindingContext suitable for hermetic tests of
// BuildSetForBinding. The cmd / pass pointers stay null — BuildSetForBinding
// doesn't need them.
PassBindingContext MakeEmptyPassContext()
{
    PassBindingContext pass{};
    pass.View          = 0;
    pass.FrameSlot     = 0;
    pass.Samples       = 1;
    return pass;
}

} // namespace

// -- Test ---------------------------------------------------------------------
// BuildSetForBinding reads the material's reflected ShaderMeta. Terrain-style
// set 2 (PatchSSBO + ParamsSSBO) with matching named DrawBindings builds a set;
// an index the meta does not declare returns an invalid handle. Two successive
// calls produce two distinct transient sets (per-draw scope, no cache), and the
// binder still builds after OnBeginFrame clears its transient caches.

TEST(MaterialBinderTests, BuildSetForBinding_PerDrawScopeIsUncached)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    Material mat = Material::TestFactory::Create(GUID::Generate(), "Terrain", 16u);
    Material::TestFactory::SetShaderMeta(mat, MakeTerrainStyleMeta());

    auto patchBuf  = MakeTestBuffer(*device, 1024, BufferUsage::Storage);
    auto paramsBuf = MakeTestBuffer(*device, 256,  BufferUsage::Storage);
    DrawBindings::BufferEntry bufs[] = {
        {HashStringId("PatchSSBO"),  patchBuf,  0, 1024},
        {HashStringId("ParamsSSBO"), paramsBuf, 0, 256},
    };
    DrawBindings draw{};
    draw.Buffers = bufs;

    auto& binder = rs.Materials().Binder();
    auto pass = MakeEmptyPassContext();
    auto setN1 = binder.BuildSetForBinding(pass, mat, 2, draw);
    ASSERT_TRUE(setN1.IsValid());

    auto setN2 = binder.BuildSetForBinding(pass, mat, 2, draw);
    ASSERT_TRUE(setN2.IsValid());
    EXPECT_NE(setN1, setN2)
        << "Per-draw scope allocates per-call — no cross-draw cache";

    EXPECT_FALSE(binder.BuildSetForBinding(pass, mat, 3, draw).IsValid())
        << "a set index the meta does not declare must not build";

    binder.OnBeginFrame();
    EXPECT_TRUE(binder.BuildSetForBinding(pass, mat, 2, draw).IsValid())
        << "the binder must still build after OnBeginFrame";

    device->DestroyBuffer(patchBuf);
    device->DestroyBuffer(paramsBuf);
    rs.Shutdown();
    device->Shutdown();
}

// -- Test ---------------------------------------------------------------------
// Bindless materials short-circuit: a shader declaring the engine's bindless
// texture set shape (single CombinedImageSampler array named
// ge_BindlessTextures) returns the global bindless descriptor set directly.

TEST(MaterialBinderTests, BindlessTextureSet_ShortCircuitsToGlobalSet)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    if (!rs.Textures().IsBindlessEnabled())
        GTEST_SKIP() << "Device does not support bindless";

    const auto globalBindless = rs.Textures().BindlessTextureSet();
    ASSERT_TRUE(globalBindless.IsValid());

    // Craft a shader meta where set=1 is the bindless texture array — the
    // canonical shape declared in shadow_sampling.glsl /
    // adapter_forward.glsl.
    auto meta = std::make_shared<ShaderMeta>();
    DescriptorSetMeta s{};
    s.Set = 1;
    DescriptorBindingMeta b{};
    b.Binding    = 0;
    b.Name       = "ge_BindlessTextures";
    b.Type       = ShaderMetaBindingType::kCombinedImageSampler;
    b.Count      = 1; // SPIR-V reflection reports unsized sampler arrays as 1.
    b.StagesMask = (1u << 0) | (1u << 1);
    s.Bindings.push_back(b);
    meta->Sets.push_back(s);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "Bindless", 16u);
    Material::TestFactory::SetShaderMeta(mat, meta);
    // The short-circuit is driven by the set's shape (IsBindlessTextureSet) +
    // TextureService::IsBindlessEnabled, not by any material variant keyword.

    auto& binder = rs.Materials().Binder();
    auto pass = MakeEmptyPassContext();
    DrawBindings draw{};

    auto h = binder.BuildSetForBinding(pass, mat, 1, draw);
    EXPECT_EQ(h, globalBindless)
        << "Bindless texture set must short-circuit to the engine's global set";

    rs.Shutdown();
    device->Shutdown();
}

// -- Test ---------------------------------------------------------------------
// Source priority: DrawBindings outranks PassResources when both have an
// entry under the same name. The set's scope escalates to PerDraw.

TEST(MaterialBinderTests, NameDriven_DrawBindingsShadowsPassResources)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto meta = std::make_shared<ShaderMeta>();
    DescriptorSetMeta s{};
    s.Set = 2;
    DescriptorBindingMeta b{};
    b.Binding    = 0;
    b.Name       = "Shared";
    b.Type       = ShaderMetaBindingType::kStorageBuffer;
    b.Count      = 1;
    b.StagesMask = (1u << 0);
    s.Bindings.push_back(b);
    meta->Sets.push_back(s);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "Shadow", 16u);
    Material::TestFactory::SetShaderMeta(mat, meta);

    auto passBuf = MakeTestBuffer(*device, 256, BufferUsage::Storage);
    auto drawBuf = MakeTestBuffer(*device, 256, BufferUsage::Storage);
    ASSERT_TRUE(passBuf.IsValid());
    ASSERT_TRUE(drawBuf.IsValid());

    auto pass = MakeEmptyPassContext();
    pass.PassResources.Buffers.push_back({HashStringId("Shared"), passBuf, 0, 0});

    DrawBindings::BufferEntry bufs[] = {{HashStringId("Shared"), drawBuf, 0, 256}};
    DrawBindings draw{};
    draw.Buffers = bufs;

    auto& binder = rs.Materials().Binder();

    auto h1 = binder.BuildSetForBinding(pass, mat, 2, draw);
    ASSERT_TRUE(h1.IsValid());

    // Per-draw scope -> a second call must allocate a fresh handle, proving
    // the classifier saw the DrawBindings entry and did NOT fall back to
    // PassResources (which would have cached).
    auto h2 = binder.BuildSetForBinding(pass, mat, 2, draw);
    ASSERT_TRUE(h2.IsValid());
    EXPECT_NE(h1, h2)
        << "DrawBindings hit must classify as per-draw (uncached), not per-pass";

    device->DestroyBuffer(passBuf);
    device->DestroyBuffer(drawBuf);
    rs.Shutdown();
    device->Shutdown();
}

// -- Test ---------------------------------------------------------------------
// Per-pass cache key must include PassInstanceIndex. Two pass contexts sharing
// (view, keywords, layout) but with different instance indices must NOT alias
// to the same cached descriptor set — otherwise shadow cascades 1..N reuse
// cascade 0's set whose Cam UBO was written from cascade 0's BufferEntry.
// RenderServices encodes the depth pass type into the high 16 bits (depth
// prepass = 0, shadow cascade 0 = 1 << 16), so the prepass and cascade 0 must
// not collide either, or cascade 0 renders with the main camera's Cam UBO.

TEST(MaterialBinderTests, PerPassCache_DistinguishesByInstanceIndex)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    // Single-binding "Cam" UBO at set 3 — classified as per-pass since the only
    // source is PassResources (no DrawBindings, no Material UBO).
    auto meta = std::make_shared<ShaderMeta>();
    DescriptorSetMeta s{};
    s.Set = 3;
    DescriptorBindingMeta b{};
    b.Binding    = 0;
    b.Name       = "Cam";
    b.Type       = ShaderMetaBindingType::kUniformBuffer;
    b.Count      = 1;
    b.StagesMask = (1u << 0);
    s.Bindings.push_back(b);
    meta->Sets.push_back(s);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "CamCascades", 16u);
    Material::TestFactory::SetShaderMeta(mat, meta);

    auto camBuf0 = MakeTestBuffer(*device, 256, BufferUsage::Uniform);
    auto camBuf1 = MakeTestBuffer(*device, 256, BufferUsage::Uniform);
    auto camCascade0 = MakeTestBuffer(*device, 256, BufferUsage::Uniform);
    ASSERT_TRUE(camBuf0.IsValid());
    ASSERT_TRUE(camBuf1.IsValid());
    ASSERT_TRUE(camCascade0.IsValid());

    auto& binder = rs.Materials().Binder();
    DrawBindings draw{};

    // Instance 0 (cascade 0) writes camBuf0 into "Cam".
    auto pass0 = MakeEmptyPassContext();
    pass0.PassInstanceIndex = 0;
    pass0.PassResources.Buffers.push_back({HashStringId("Cam"), camBuf0, 0, 0});
    auto h0 = binder.BuildSetForBinding(pass0, mat, 3, draw);
    ASSERT_TRUE(h0.IsValid());

    // Instance 1 (cascade 1) writes camBuf1 into "Cam" but shares (view,
    // keywords, layout) with pass0. Must NOT reuse pass0's handle — otherwise
    // cascade 1 renders with cascade 0's camera.
    auto pass1 = MakeEmptyPassContext();
    pass1.PassInstanceIndex = 1;
    pass1.PassResources.Buffers.push_back({HashStringId("Cam"), camBuf1, 0, 0});
    auto h1 = binder.BuildSetForBinding(pass1, mat, 3, draw);
    ASSERT_TRUE(h1.IsValid());
    EXPECT_NE(h0, h1)
        << "Per-pass cache must key on PassInstanceIndex so shadow cascades "
           "don't alias to cascade 0's descriptor set";

    // Shadow cascade 0 as RenderServices encodes it: PassType::ShadowCascade (1) << 16.
    auto passCascade0 = MakeEmptyPassContext();
    passCascade0.PassInstanceIndex = (static_cast<uint32_t>(1) << 16) | 0u;
    passCascade0.PassResources.Buffers.push_back({HashStringId("Cam"), camCascade0, 0, 0});
    auto hCascade0 = binder.BuildSetForBinding(passCascade0, mat, 3, draw);
    ASSERT_TRUE(hCascade0.IsValid());
    EXPECT_NE(h0, hCascade0) << "the depth prepass (0) and shadow cascade 0 (1 << 16) must not share a set";
    EXPECT_NE(h1, hCascade0);

    // Same instance index in a fresh context still hits the cache.
    auto pass0Again = MakeEmptyPassContext();
    pass0Again.PassInstanceIndex = 0;
    pass0Again.PassResources.Buffers.push_back({HashStringId("Cam"), camBuf0, 0, 0});
    auto h0Again = binder.BuildSetForBinding(pass0Again, mat, 3, draw);
    EXPECT_EQ(h0, h0Again);

    device->DestroyBuffer(camBuf0);
    device->DestroyBuffer(camBuf1);
    device->DestroyBuffer(camCascade0);
    rs.Shutdown();
    device->Shutdown();
}

// -- Test ---------------------------------------------------------------------
// The per-pass L1 is thread_local, so its slots outlive every binder that fills
// them. A slot is only safe to trust if what stamps it cannot recur once its
// writer is gone — an address recurs as soon as the allocator hands it back, and
// a per-binder frame counter starts from the same value in every binder, so
// neither identifies the writer on its own.
//
// Both binders are placement-constructed in one buffer to make that address
// reuse exact instead of incidental, and share one device so that its
// monotonically increasing descriptor-set ids make a real build impossible to
// confuse with a handle served out of a slot the dead binder left behind.

TEST(MaterialBinderTests, PerPassL1_DoesNotServeADeadBindersHandle)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto meta = std::make_shared<ShaderMeta>();
    DescriptorSetMeta s{};
    s.Set = 3;
    DescriptorBindingMeta b{};
    b.Binding    = 0;
    b.Name       = "Cam";
    b.Type       = ShaderMetaBindingType::kUniformBuffer;
    b.Count      = 1;
    b.StagesMask = (1u << 0);
    s.Bindings.push_back(b);
    meta->Sets.push_back(s);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "L1OwnerRecycle", 16u);
    Material::TestFactory::SetShaderMeta(mat, meta);

    auto camBuf = MakeTestBuffer(*device, 256, BufferUsage::Uniform);
    ASSERT_TRUE(camBuf.IsValid());

    DrawBindings draw{};
    const auto makePass = [&]() {
        auto pass = MakeEmptyPassContext();
        pass.PassInstanceIndex = 0;
        pass.PassResources.Buffers.push_back({HashStringId("Cam"), camBuf, 0, 0});
        return pass;
    };

    alignas(MaterialBinder) std::byte storage[sizeof(MaterialBinder)];

    auto* first = new (storage) MaterialBinder(rs, *device);
    auto passFirst = makePass();
    const auto hFirst = first->BuildSetForBinding(passFirst, mat, 3, draw);
    ASSERT_TRUE(hFirst.IsValid());
    first->~MaterialBinder();

    auto* second = new (storage) MaterialBinder(rs, *device);
    auto passSecond = makePass();
    const auto hSecond = second->BuildSetForBinding(passSecond, mat, 3, draw);
    ASSERT_TRUE(hSecond.IsValid());
    second->~MaterialBinder();

    EXPECT_NE(hFirst, hSecond)
        << "A per-pass L1 slot filled by a destroyed binder was served to a new "
           "binder occupying its address. The new binder's own per-pass cache is "
           "empty, so it never built this handle, and on a device whose "
           "descriptor-set ids only increase a genuine build cannot return the "
           "earlier one. Whatever validates an L1 slot must not be a value a "
           "later binder can repeat.";

    device->DestroyBuffer(camBuf);
    rs.Shutdown();
    device->Shutdown();
}

// -- Test ---------------------------------------------------------------------
// Device-lost hole closure (twice-fatal: grass 'Grass' UBO + C3 EditorPreview).
// A statically-used set-2 uniform/storage buffer that resolves from neither
// DrawBindings nor PassResources must set the outUnresolvable flag, so
// Preparation rejects the draw instead of issuing it with an unbound
// descriptor — which under descriptor buffers is a garbage device address ->
// fetch fault -> device lost. When a provider exists, the flag stays false.
TEST(MaterialBinderTests, BuildSetForBindingFromMeta_FlagsUnresolvableStaticBuffer)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    // Set 2 declaring one SSBO with a name no source provides by default.
    DescriptorSetMeta setMeta{};
    setMeta.Set = 2;
    DescriptorBindingMeta b{};
    b.Binding    = 0;
    b.Name       = "NeedsProvider";
    b.Type       = ShaderMetaBindingType::kStorageBuffer;
    b.Count      = 1;
    b.StagesMask = 1u;
    setMeta.Bindings.push_back(b);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "Unresolvable", 16u);
    Material::TestFactory::SetShaderMeta(mat, std::make_shared<ShaderMeta>());

    auto& binder = rs.Materials().Binder();

    // No provider anywhere -> flagged unresolvable (draw would be skipped).
    {
        auto pass = MakeEmptyPassContext();
        DrawBindings draw{};
        bool unresolvable = false;
        binder.BuildSetForBindingFromMeta(pass, mat, setMeta, draw, &unresolvable);
        EXPECT_TRUE(unresolvable)
            << "an unprovided statically-used SSBO must flag unresolvable so the draw is skipped "
               "instead of binding a garbage-address descriptor";
    }

    // Provider present in DrawBindings -> resolves, flag stays false.
    {
        auto buf = MakeTestBuffer(*device, 256, BufferUsage::Storage);
        ASSERT_TRUE(buf.IsValid());
        DrawBindings::BufferEntry bufs[] = {{HashStringId("NeedsProvider"), buf, 0, 256}};
        auto pass = MakeEmptyPassContext();
        DrawBindings draw{};
        draw.Buffers = bufs;
        bool unresolvable = false;
        auto h = binder.BuildSetForBindingFromMeta(pass, mat, setMeta, draw, &unresolvable);
        EXPECT_FALSE(unresolvable) << "a provided SSBO must not flag unresolvable";
        EXPECT_TRUE(h.IsValid());
        device->DestroyBuffer(buf);
    }

    rs.Shutdown();
    device->Shutdown();
}

// Missing sampled images use a default with the reflected view shape.
// Depth defaults use the shadow comparison sampler; color defaults use the material sampler.

namespace
{
DescriptorBindingMeta ImageBinding(const char* name, bool depth, bool arrayed, bool cube, bool multisample = false)
{
    DescriptorBindingMeta binding{};
    binding.Binding    = 9;
    binding.Name       = name;
    binding.Type       = ShaderMetaBindingType::kCombinedImageSampler;
    binding.StagesMask = ShaderMetaStage::kFragment;
    DescriptorBindingMeta::ImageInfo image{};
    image.Dim         = 2;
    image.Depth       = depth;
    image.Arrayed     = arrayed;
    image.Cube        = cube;
    image.Multisample = multisample;
    binding.Image = image;
    return binding;
}

void ExpectImageFallbackMatchesProvidedResources(MaterialBinder& binder, const Material& material,
                                                const DescriptorBindingMeta& binding,
                                                TextureHandle texture, SamplerHandle sampler,
                                                SamplerHandle differentSampler)
{
    SCOPED_TRACE(binding.Name);
    ASSERT_TRUE(texture.IsValid());
    ASSERT_TRUE(sampler.IsValid());
    ASSERT_TRUE(differentSampler.IsValid());
    ASSERT_NE(sampler, differentSampler);
    DescriptorSetMeta set{};
    set.Set = 2;
    set.Bindings.push_back(binding);
    FinalizeSetLayout(set);
    auto pass = MakeEmptyPassContext();
    bool unresolvable = false;
    const auto fallback = binder.BuildSetForBindingFromMeta(pass, material, set, {}, &unresolvable);
    ASSERT_FALSE(unresolvable);
    ASSERT_TRUE(fallback.IsValid());

    pass.PassResources.Textures.push_back({binding.ResolvedNameId(), texture, sampler});
    const auto provided = binder.BuildSetForBindingFromMeta(pass, material, set, {}, &unresolvable);
    EXPECT_FALSE(unresolvable);
    EXPECT_EQ(fallback, provided)
        << "the fallback and explicit expected resources must resolve to the same descriptor contents";
    pass.PassResources.Textures.front().Sampler = differentSampler;
    EXPECT_NE(fallback, binder.BuildSetForBindingFromMeta(pass, material, set, {}))
        << "a different sampler must prevent reuse of the fallback descriptor";
}
} // namespace

TEST(MaterialBinderTests, BuildSetForBindingFromMeta_UsesTheSlotShapeFallback)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices services;
    ASSERT_TRUE(services.Initialize(device.get()));
    auto& binder = services.Materials().Binder();
    const Material material = Material::TestFactory::Create(GUID::Generate(), "SlotFallback", 16u);
    const SamplerHandle materialSampler = services.Textures().GetSampler(material.GetSamplerPreset());
    ASSERT_TRUE(materialSampler.IsValid());
    const auto shadowSampler = services.GetCascadeShadowSampler();
    ASSERT_NE(materialSampler, shadowSampler);
    const auto pointSampler = services.Textures().GetSampler(SamplerPreset::PointClamp);

    const auto depthArray = services.GetCascadeShadowFallbackTexture();
    EXPECT_EQ(device->GetTextureFormat(depthArray), TextureFormat::D32_FLOAT);
    EXPECT_GT(device->GetTextureArrayLayers(depthArray), 1u);
    ExpectImageFallbackMatchesProvidedResources(binder, material,
        ImageBinding("ge_shadowMapArray", true, true, false), depthArray, shadowSampler, pointSampler);

    const auto depth2D = services.GetAreaShadowFallbackTexture();
    EXPECT_EQ(device->GetTextureFormat(depth2D), TextureFormat::D32_FLOAT);
    EXPECT_EQ(device->GetTextureArrayLayers(depth2D), 1u);
    ExpectImageFallbackMatchesProvidedResources(binder, material,
        ImageBinding("ge_areaShadowMap", true, false, false), depth2D, shadowSampler, pointSampler);

    const auto cube = services.Textures().GetDefaultBlackCubeTexture();
    EXPECT_EQ(device->GetTextureArrayLayers(cube), 6u);
    ExpectImageFallbackMatchesProvidedResources(binder, material,
        ImageBinding("ge_irradianceCubeTex", false, false, true), cube, materialSampler, pointSampler);
    ExpectImageFallbackMatchesProvidedResources(binder, material,
        ImageBinding("ge_shadowMomentsArray", false, true, false),
        services.Textures().GetDefaultWhiteArrayTexture(), materialSampler, pointSampler);
    ExpectImageFallbackMatchesProvidedResources(binder, material,
        ImageBinding("ge_brdfLUTTex", false, false, false),
        services.Textures().ResolveDefaultTexture("ge_brdfLUTTex"), materialSampler, pointSampler);

    const std::array unsupported{
        ImageBinding("ge_sceneDepthMS", false, false, false, true),
        ImageBinding("ge_depthCube", true, false, true)};
    for (const auto& binding : unsupported)
    {
        SCOPED_TRACE(binding.Name);
        DescriptorSetMeta set{};
        set.Set = 2;
        set.Bindings.push_back(binding);
        auto pass = MakeEmptyPassContext();
        bool unresolvable = false;
        binder.BuildSetForBindingFromMeta(pass, material, set, {}, &unresolvable);
        EXPECT_TRUE(unresolvable) << "the reflected shape has no default resource";
    }

    services.Shutdown();
    device->Shutdown();
}

// A sampled-image slot whose shape has no default (here a 3D texture) cannot be written, and an
// unwritten entry is an invalid bind group on WebGPU and a garbage descriptor under descriptor
// buffers: the set flags unresolvable so the draw is skipped, the same rule as an unbound buffer.
TEST(MaterialBinderTests, BuildSetForBindingFromMeta_FlagsAnUnresolvedSlotWithNoDefaultShape)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto& binder = rs.Materials().Binder();
    Material mat = Material::TestFactory::Create(GUID::Generate(), "NoDefaultShape", 16u);
    Material::TestFactory::SetShaderMeta(mat, std::make_shared<ShaderMeta>());

    DescriptorSetMeta setMeta{};
    setMeta.Set = 2;
    DescriptorBindingMeta volume = ImageBinding("gVolumeNobodyProvides", false, false, false);
    volume.Image->Dim = 3;
    setMeta.Bindings.push_back(volume);
    {
        auto pass = MakeEmptyPassContext();
        DrawBindings draw{};
        bool unresolvable = false;
        binder.BuildSetForBindingFromMeta(pass, mat, setMeta, draw, &unresolvable);
        EXPECT_TRUE(unresolvable) << "a slot with no default of its shape must skip the draw, not issue it unwritten";
    }

    // A shape that has a default resolves to it and leaves the draw in.
    setMeta.Bindings = {ImageBinding("ge_shadowMapArray", true, true, false)};
    {
        auto pass = MakeEmptyPassContext();
        DrawBindings draw{};
        bool unresolvable = false;
        binder.BuildSetForBindingFromMeta(pass, mat, setMeta, draw, &unresolvable);
        EXPECT_FALSE(unresolvable) << "a depth-array slot binds its default";
    }

    rs.Shutdown();
    device->Shutdown();
}

// -- InvalidateStickyBindsForPipeline -----------------------------------------
// Vulkan pipeline-layout-compatibility rule: when a pipeline switch keeps the
// set-N layout but changes set-(N+1)'s, only set-(N+1) and above are disturbed.
// MaterialBinder mirrors that on its sticky-bind tracking so the next descriptor
// walk only rebinds what the GPU actually invalidated. These tests pin the rule
// so a future change that over-clears (regressing back to "fill({}) on every
// pipeline change") is caught instead of silently re-introducing redundant
// vkCmdBindDescriptorSets calls in the per-draw hot path.

namespace
{

// Synthesize a fully-populated PassBindingContext so we can observe which
// slots InvalidateStickyBindsForPipeline preserves vs clears. Sets / layouts
// at indices [0..3] get distinct non-zero values; [4..7] stay default.
PassBindingContext MakePopulatedPassContext()
{
    PassBindingContext pass{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        // Distinct, non-zero set handles + layout ids so a missed clear is
        // observable as the original value surviving.
        pass.CurrentSets[i]       = DescriptorSetHandle{0x100ull + i};
        pass.CurrentSetLayouts[i] = DescriptorSetLayoutId{1u + i};
    }
    return pass;
}

} // namespace

TEST(MaterialBinderTests, InvalidateStickyBinds_FirstDiffPreservesLowerSetsClearsHigher)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto& binder = rs.Materials().Binder();
    auto pass = MakePopulatedPassContext();

    // New pipeline shares set-0's layout (id=1) but set-1's layout differs (id=99).
    // Vulkan keeps set-0 sticky and disturbs sets [1..). Set-2 / set-3 had
    // distinct layouts on the prior pipeline; they're all cleared regardless
    // because firstDiff is the lowest mismatching set.
    const DescriptorSetLayoutId newLayouts[] = {
        DescriptorSetLayoutId{1u},   // same as prior pipeline -> set-0 sticky
        DescriptorSetLayoutId{99u},  // differs -> firstDiff
    };
    binder.InvalidateStickyBindsForPipeline(pass, newLayouts);

    EXPECT_EQ(pass.CurrentSets[0], DescriptorSetHandle{0x100ull})
        << "Set-0 layout unchanged across pipeline change; sticky bind must survive";
    for (uint32_t i = 1; i < kMaxDescriptorSets; ++i)
    {
        EXPECT_FALSE(pass.CurrentSets[i].IsValid())
            << "Set " << i << " is at or above firstDiff -> sticky bind must be cleared";
    }

    // Layout tracking: untouched below firstDiff (still {1}); rewritten from
    // firstDiff upward (set-1 -> {99}; sets without an entry in the new layouts
    // span fall back to default-constructed).
    EXPECT_EQ(pass.CurrentSetLayouts[0], DescriptorSetLayoutId{1u});
    EXPECT_EQ(pass.CurrentSetLayouts[1], DescriptorSetLayoutId{99u});
    for (uint32_t i = 2; i < kMaxDescriptorSets; ++i)
    {
        EXPECT_FALSE(pass.CurrentSetLayouts[i].IsValid())
            << "Slots beyond the new pipeline's layout span must clear to default";
    }

    rs.Shutdown();
    device->Shutdown();
}

TEST(MaterialBinderTests, InvalidateStickyBinds_AllLayoutsMatchPreservesEverySet)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto& binder = rs.Materials().Binder();
    auto pass = MakePopulatedPassContext();

    // Same layout ids as the populated context for slots [0..3]; trailing
    // slots stay default, which matches the prior pipeline's defaults too.
    const DescriptorSetLayoutId newLayouts[] = {
        DescriptorSetLayoutId{1u},
        DescriptorSetLayoutId{2u},
        DescriptorSetLayoutId{3u},
        DescriptorSetLayoutId{4u},
    };
    binder.InvalidateStickyBindsForPipeline(pass, newLayouts);

    for (uint32_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(pass.CurrentSets[i], DescriptorSetHandle{0x100ull + i})
            << "Identical layout at every slot must preserve every sticky bind (set " << i << ")";
        EXPECT_EQ(pass.CurrentSetLayouts[i], DescriptorSetLayoutId{1u + i});
    }

    rs.Shutdown();
    device->Shutdown();
}

TEST(MaterialBinderTests, InvalidateStickyBinds_FirstSlotDiffersClearsAllSets)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto& binder = rs.Materials().Binder();
    auto pass = MakePopulatedPassContext();

    // Set-0 layout differs -> firstDiff is 0, every set is disturbed.
    const DescriptorSetLayoutId newLayouts[] = {
        DescriptorSetLayoutId{42u},
    };
    binder.InvalidateStickyBindsForPipeline(pass, newLayouts);

    for (uint32_t i = 0; i < kMaxDescriptorSets; ++i)
    {
        EXPECT_FALSE(pass.CurrentSets[i].IsValid())
            << "Set-0 layout differs -> Vulkan disturbs all sets, sticky bind " << i << " must clear";
    }
    EXPECT_EQ(pass.CurrentSetLayouts[0], DescriptorSetLayoutId{42u});
    for (uint32_t i = 1; i < kMaxDescriptorSets; ++i)
    {
        EXPECT_FALSE(pass.CurrentSetLayouts[i].IsValid());
    }

    rs.Shutdown();
    device->Shutdown();
}

TEST(MaterialBinderTests, InvalidateStickyBinds_EmptyLayoutsConservativelyClearsAll)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto& binder = rs.Materials().Binder();
    auto pass = MakePopulatedPassContext();

    // Callers that didn't precompute layout ids fall back to clearing every
    // slot — same behavior as before the PR #74 smart-clear optimisation.
    binder.InvalidateStickyBindsForPipeline(
        pass, std::span<const DescriptorSetLayoutId>{});

    for (uint32_t i = 0; i < kMaxDescriptorSets; ++i)
    {
        EXPECT_FALSE(pass.CurrentSets[i].IsValid())
            << "Empty layout span must clear every sticky bind (legacy fallback)";
        EXPECT_FALSE(pass.CurrentSetLayouts[i].IsValid())
            << "Empty layout span must also clear layout tracking";
    }

    rs.Shutdown();
    device->Shutdown();
}

// -- Variant-cache cross-thread eviction queue --------------------------------
// The four variant caches in RenderServices are written from the render thread
// only. Material unregister callbacks can fire from any thread (asset reloader,
// GC worker); they hand off via EnqueueVariantCacheEviction, and the render
// thread drains at BeginWorldDrawFrame. These tests pin the contract:
//   1. Enqueue is safe under concurrent writers.
//   2. The drain is monotonic — every enqueued pointer is consumed exactly
//      once per BeginWorldDrawFrame call, no losses, no duplicates remaining.
// Drain itself is single-threaded by design, so we don't race readers/writers
// in the test — that's the very contract the deferred queue is here to enforce.

TEST(MaterialBinderTests, VariantCacheEviction_ConcurrentEnqueueAccumulates)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    ASSERT_EQ(rs.Materials().Variants().PendingEvictionCount(), 0u);

    // Fan out across 4 worker threads, each enqueueing 64 sentinel pointers.
    // Pointers are never dereferenced (Drain only does address comparison
    // against the cache key.material), so we can fabricate them from
    // integers — the real unregister path uses the actual Material*.
    constexpr int kThreads = 4;
    constexpr int kPerThread = 64;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kPerThread; ++i)
            {
                const uintptr_t bits = (static_cast<uintptr_t>(t) << 32) | static_cast<uintptr_t>(i + 1);
                rs.Materials().Variants().EnqueueEviction(reinterpret_cast<const Material*>(bits));
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < kThreads)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);

    for (auto& w : workers)
        w.join();

    // Total = kThreads * kPerThread; mutex serialisation guarantees no losses.
    EXPECT_EQ(rs.Materials().Variants().PendingEvictionCount(),
              static_cast<size_t>(kThreads * kPerThread))
        << "Concurrent enqueues must accumulate — mutex must protect push_back";

    rs.Shutdown();
    device->Shutdown();
}

TEST(MaterialBinderTests, VariantCacheEviction_BeginWorldDrawFrameDrainsQueue)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    // Enqueue from this thread (the test's main thread, which will also play
    // the role of render thread when we call BeginWorldDrawFrame). Variant
    // caches are empty, so Drain finds nothing to erase — what we're pinning
    // here is that the queue itself is drained back to zero each frame.
    for (uintptr_t i = 1; i <= 32; ++i)
        rs.Materials().Variants().EnqueueEviction(reinterpret_cast<const Material*>(i));

    ASSERT_EQ(rs.Materials().Variants().PendingEvictionCount(), 32u);

    rs.BeginWorldDrawFrame();

    EXPECT_EQ(rs.Materials().Variants().PendingEvictionCount(), 0u)
        << "BeginWorldDrawFrame must drain the entire pending eviction queue";

    // Calling again on an empty queue must remain a no-op (Drain early-exits
    // when there's nothing pending).
    rs.BeginWorldDrawFrame();
    EXPECT_EQ(rs.Materials().Variants().PendingEvictionCount(), 0u);

    rs.Shutdown();
    device->Shutdown();
}

namespace
{
class MaterialBindingPlanTests : public ::testing::Test
{
  protected:
    virtual GraphicsAPI RequestedAPI() const { return GraphicsAPI::Vulkan; }
    void CheckBufferReadback();
    void CheckForwardCommandPreparation(bool worldPass = false);
    void CheckForwardSpanOwnership(bool worldPass, bool afterDeclaration);
    void CheckDepthCommandPreparation();

    void SetUp() override
    {
        DeviceDesc desc{};
        desc.preferredAPI = RequestedAPI();
        desc.enableSwapchain = false;
        desc.enableDebugLayer = false;
        desc.pipelineCacheDirectory = m_Cache.Path().string();
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            GTEST_SKIP() << "Requested device unavailable";
        ASSERT_EQ(m_Device->GetAPI(), RequestedAPI());
        ASSERT_TRUE(m_Services.Initialize(m_Device.get()));
    }

    void TearDown() override
    {
        m_Services.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    static DescriptorSetMeta MakeSet(std::string name, uint32_t type, uint32_t index = 0)
    {
        DescriptorSetMeta set{};
        set.Set = index;
        DescriptorBindingMeta slot{};
        slot.Name = std::move(name);
        slot.Type = type;
        slot.StagesMask = ShaderMetaStage::kFragment;
        set.Bindings.push_back(std::move(slot));
        FinalizeSetLayout(set);
        return set;
    }

    TestUtils::ScopedTempDir m_Cache{TestUtils::MakeUniqueTempDirectory("lucia-material-binding-plans")};
    std::unique_ptr<IDevice> m_Device;
    RenderServices m_Services;
    Material m_Material = Material::TestFactory::Create(GUID::Generate(), "BindingPlan", 16u);
};
} // namespace

TEST_F(MaterialBindingPlanTests, ServedSetUsesItsProviderAndExactBufferSlice)
{
    auto camera = MakeSet("Camera", ShaderMetaBindingType::kUniformBuffer);
    auto light = MakeSet("Light", ShaderMetaBindingType::kUniformBuffer);
    auto moved = MakeSet("Camera", ShaderMetaBindingType::kUniformBuffer, 2);
    const auto first = MakeTestBuffer(*m_Device, 1024, BufferUsage::Uniform);
    const auto second = MakeTestBuffer(*m_Device, 1024, BufferUsage::Uniform);
    ASSERT_TRUE(first.IsValid() && second.IsValid());
    auto pass = MakeEmptyPassContext();
    pass.PassResources.Buffers = {{HashStringId("Camera"), first, 0, 256},
                                  {HashStringId("Light"), second, 0, 256}};
    auto& binder = m_Services.Materials().Binder();
    auto build = [&](const DescriptorSetMeta& set) {
        return binder.BuildSetForBindingFromMeta(pass, m_Material, set, {});
    };
    const auto original = build(camera);
    ASSERT_TRUE(original.IsValid());
    EXPECT_NE(original, build(light));
    pass.PassResources.Buffers.back().Buffer = first;
    EXPECT_NE(original, build(light)) << "two provider names with identical physical writes";
    auto sameCamera = MakeSet("Camera", ShaderMetaBindingType::kUniformBuffer);
    EXPECT_EQ(original, build(sameCamera)) << "independently decoded equivalent plans can share";
    EXPECT_NE(original, build(moved));
    pass.PassResources.Buffers.front().Offset = 256;
    const auto offset = build(camera);
    EXPECT_NE(original, offset);
    pass.PassResources.Buffers.front().Range = 128;
    EXPECT_NE(offset, build(camera));
    pass.PassResources.Buffers.front() = {HashStringId("Camera"), second, 0, 256};
    EXPECT_NE(original, build(camera));
    pass.PassResources.Buffers.front() = {HashStringId("Camera"), first, 0, 256};
    EXPECT_EQ(original, build(camera));
    pass.PassResources.Buffers.clear();
    bool missing = false;
    binder.BuildSetForBindingFromMeta(pass, m_Material, camera, {}, &missing);
    EXPECT_TRUE(missing);
    m_Device->DestroyBuffer(first);
    m_Device->DestroyBuffer(second);
}

TEST_F(MaterialBindingPlanTests, SplitSamplerOverridesArePerDrawAndDoNotChangePassSet)
{
    const auto set = MakeSet("SurfaceSampler", ShaderMetaBindingType::kSampler);
    const auto name = HashStringId("SurfaceSampler");
    auto pass = MakeEmptyPassContext();
    auto& binder = m_Services.Materials().Binder();
    const auto fallback = binder.BuildSetForBindingFromMeta(pass, m_Material, set, {});
    const auto point = m_Services.Textures().GetSampler(SamplerPreset::PointClamp);
    ASSERT_TRUE(point.IsValid() && fallback.IsValid());
    pass.PassResources.Textures.push_back({name, {}, point});
    const auto passSet = binder.BuildSetForBindingFromMeta(pass, m_Material, set, {});
    EXPECT_NE(fallback, passSet);
    DrawBindings::TextureEntry sampler{name, {}, m_Services.Textures().GetSampler(SamplerPreset::LinearClamp)};
    DrawBindings draw{};
    draw.Textures = {&sampler, 1};
    const auto overrideSet = binder.BuildSetForBindingFromMeta(pass, m_Material, set, draw);
    EXPECT_NE(passSet, overrideSet);
    EXPECT_NE(overrideSet, binder.BuildSetForBindingFromMeta(pass, m_Material, set, draw));
    EXPECT_EQ(passSet, binder.BuildSetForBindingFromMeta(pass, m_Material, set, {}));
}

TEST_F(MaterialBindingPlanTests, ImageLayoutAndShapeFallbackFollowTheServedReflection)
{
    auto flat = MakeSet("Image", ShaderMetaBindingType::kSampledImage);
    auto cube = flat;
    cube.Bindings.front().Image.emplace().Cube = true;
    FinalizeSetLayout(cube);
    auto unsupported = flat;
    unsupported.Bindings.front().Image.emplace().Dim = 3;
    FinalizeSetLayout(unsupported);
    auto pass = MakeEmptyPassContext();
    auto& binder = m_Services.Materials().Binder();
    const auto first = binder.BuildSetForBindingFromMeta(pass, m_Material, flat, {});
    const auto second = binder.BuildSetForBindingFromMeta(pass, m_Material, cube, {});
    ASSERT_TRUE(first.IsValid() && second.IsValid());
    EXPECT_NE(first, second);
    bool missing = false;
    binder.BuildSetForBindingFromMeta(pass, m_Material, unsupported, {}, &missing);
    EXPECT_TRUE(missing);
    EXPECT_EQ(first, binder.BuildSetForBindingFromMeta(pass, m_Material, flat, {}));
}

TEST_F(MaterialBindingPlanTests, CombinedImageFollowsAnEditedMaterialSampler)
{
    auto doc = MaterialDocument::CreateDefaultPBR("FilteredImage");
    const auto guid = GUID::Generate();
    Material* edited = m_Services.Materials().Registry().Register(guid, doc);
    ASSERT_NE(edited, nullptr);
    const auto set = MakeSet("Image", ShaderMetaBindingType::kCombinedImageSampler);
    auto pass = MakeEmptyPassContext();
    pass.PassResources.Textures.push_back({HashStringId("Image"), m_Services.Textures().GetDefaultWhiteTexture(), {}});
    auto& binder = m_Services.Materials().Binder();
    const auto linear = binder.BuildSetForBindingFromMeta(pass, *edited, set, {});
    doc.textureFilter = MaterialTextureFilter::Point;
    m_Services.Textures().UpdateMaterialTextures(guid, doc);
    ASSERT_NE(edited->GetSamplerPreset(), SamplerPreset::LinearRepeat);
    const auto point = binder.BuildSetForBindingFromMeta(pass, *edited, set, {});
    ASSERT_TRUE(linear.IsValid() && point.IsValid());
    EXPECT_NE(linear, point);
    pass.PassResources.Textures.front().Texture = m_Services.Textures().GetDefaultBlackTexture();
    EXPECT_NE(point, binder.BuildSetForBindingFromMeta(pass, *edited, set, {}));
}

// ubo_copy copies one float from its UBO to its storage buffer. Both dispatches
// share the pass, material and layout. Only the actual UBO slice changes.
void MaterialBindingPlanTests::CheckBufferReadback()
{
    const auto shader = Utils::LoadShaderFile("Shaders/ubo_copy.comp.spv");
    ASSERT_FALSE(shader.empty());
    StageReflectionResult stage;
    std::string error;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, reinterpret_cast<const uint32_t*>(shader.data()),
                             shader.size() / sizeof(uint32_t), {}, stage, &error)) << error;
    const auto meta = MergeStages({stage});
    ASSERT_EQ(meta.Sets.size(), 1u);
    const auto& set = meta.Sets.front();
    ASSERT_EQ(set.Bindings.size(), 2u);
    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Compute;
    pipelineDesc.computeShader = shader;
    pipelineDesc.descriptorSetLayouts = MaterialBuilder::BuildSetLayouts(meta);
    const auto pipeline = m_Device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    ASSERT_TRUE(m_Device->BeginFrame());
    const auto source = MakeTestBuffer(*m_Device, 512, BufferUsage::Uniform);
    BufferDesc outDesc{};
    outDesc.size = sizeof(float);
    outDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc);
    const auto output = m_Device->CreateBuffer(outDesc);
    BufferDesc readbackDesc{};
    readbackDesc.size = 2 * sizeof(float);
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    const auto readback = m_Device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(source.IsValid() && output.IsValid() && readback.IsValid());
    constexpr std::array<float, 2> kExpected{3.0f, 17.0f};
    auto pass = MakeEmptyPassContext();
    for (const auto& binding : set.Bindings)
    {
        const bool input = binding.Binding == 0;
        pass.PassResources.Buffers.push_back({binding.ResolvedNameId(), input ? source : output,
                                              0, input ? 16u : sizeof(float)});
    }
    auto& binder = m_Services.Materials().Binder();
    std::array<PreparedDrawBindings, kExpected.size()> packets{};
    ASSERT_EQ(pass.Cmd, nullptr);
    for (size_t i = 0; i < packets.size(); ++i)
    {
        const uint64_t offset = i * 256;
        m_Device->UpdateBuffer(source, offset, sizeof(float), &kExpected[i]);
        for (auto& buffer : pass.PassResources.Buffers)
            if (buffer.Buffer == source)
                buffer.Offset = offset;
        packets[i] = binder.PrepareMaterialForDraw(pass, m_Material, pipeline, &meta, {});
        ASSERT_TRUE(packets[i].Pipeline.IsValid());
        ASSERT_TRUE(packets[i].Sets[0].IsValid());
    }
    EXPECT_NE(packets[0].Sets[0], packets[1].Sets[0]);
    EXPECT_EQ(pass.PipelineBinds, 0u);
    EXPECT_EQ(pass.DescriptorBinds, 0u);
    pass.PassResources.Buffers.clear();

    auto command = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(command);
    command->Begin();
    for (size_t i = 0; i < packets.size(); ++i)
    {
        // CopyBuffer closes the compute encoder on Metal. Each new encoder
        // needs its own sticky state and an explicit compute pipeline bind.
        PassBindingContext record{};
        record.Cmd = command.get();
        EXPECT_EQ(binder.BindPreparedForDraw(record, packets[i]), pipeline);
        EXPECT_EQ(binder.BindPreparedForDraw(record, packets[i]), pipeline);
        EXPECT_EQ(record.PipelineBinds, 1u);
        EXPECT_EQ(record.DescriptorBinds, 1u);
        command->Dispatch(1, 1, 1);
        command->Barrier(ResourceBarrier::CreateBufferBarrier(output, ResourceState::UnorderedAccess, ResourceState::CopySource));
        command->CopyBuffer(output, readback, sizeof(float), 0, i * sizeof(float));
        command->Barrier(ResourceBarrier::CreateBufferBarrier(output, ResourceState::CopySource, ResourceState::UnorderedAccess));
    }
    command->End();
    m_Device->ExecuteCommandLists({command.get()});
    m_Device->FinalizeFrame();
    m_Device->WaitForIdle();

    const auto* values = static_cast<const float*>(m_Device->MapBuffer(readback));
    ASSERT_NE(values, nullptr);
    EXPECT_FLOAT_EQ(values[0], kExpected[0]);
    EXPECT_FLOAT_EQ(values[1], kExpected[1]);
    m_Device->UnmapBuffer(readback);
    m_Device->DestroyBuffer(readback);
    m_Device->DestroyBuffer(output);
    m_Device->DestroyBuffer(source);
    m_Device->DestroyPipeline(pipeline);
}

void MaterialBindingPlanTests::CheckForwardCommandPreparation(bool worldPass)
{
    namespace RG = GameEngine::Rendering::RenderGraph;
    const auto vertex = Utils::LoadShaderFile("Shaders/fullscreen_noinput.vert.spv");
    const auto fragment = Utils::LoadShaderFile("Shaders/ubo_sample.frag.spv");
    ASSERT_FALSE(vertex.empty());
    ASSERT_FALSE(fragment.empty());
    StageReflectionResult stage;
    std::string error;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fragment.data()),
                             fragment.size() / sizeof(uint32_t), {}, stage, &error)) << error;
    const auto meta = MergeStages({stage});
    ASSERT_EQ(meta.Sets.size(), 1u);
    ASSERT_EQ(meta.Sets.front().Bindings.size(), 1u);
    GraphicsPipelineDesc pipeline{};
    pipeline.VertexShader = std::make_shared<const std::vector<uint8_t>>(vertex);
    pipeline.PixelShader = std::make_shared<const std::vector<uint8_t>>(fragment);
    pipeline.Rasterization.cullMode = GameEngine::Rendering::CullModeFlagBits::None;
    pipeline.ColorBlend.attachments.emplace_back();
    for (const auto& layout : MaterialBuilder::BuildSetLayouts(meta))
        pipeline.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    const auto id = m_Device->InternGraphicsPipeline(pipeline);
    ASSERT_TRUE(id.IsValid());
    PipelineFormatKey format{};
    format.ColorCount = 1;
    format.ColorFormats[0] = TextureFormat::RGBA8_UNORM;
    ASSERT_FALSE(m_Device->TryGetWarmGraphicsPipeline(id, format).IsValid());

    ASSERT_TRUE(m_Device->BeginFrame());
    const auto uniform = MakeTestBuffer(*m_Device, 512, BufferUsage::Uniform);
    const std::array<uint16_t, 6> triangles{0, 0, 0, 0, 1, 2};
    const auto indices = MakeTestBuffer(*m_Device, sizeof(triangles), BufferUsage::Index);
    ASSERT_TRUE(uniform.IsValid() && indices.IsValid());
    constexpr float kBlue = 0.0f, kGreen = 1.0f;
    m_Device->UpdateBuffer(uniform, 0, sizeof(kBlue), &kBlue);
    m_Device->UpdateBuffer(uniform, 256, sizeof(kGreen), &kGreen);
    m_Device->UpdateBuffer(indices, 0, sizeof(triangles), triangles.data());
    const auto name = meta.Sets.front().Bindings.front().ResolvedNameId();
    std::array<DrawBindings::BufferEntry, 2> overrides{{{name, uniform, 0, 208}, {name, uniform, 256, 208}}};
    std::array<DrawCommand, 2> commands{};
    for (size_t i = 0; i < commands.size(); ++i)
    {
        commands[i].InternedPipeline = id;
        commands[i].PipelineMeta = &meta;
        commands[i].Geometry.AltGeom.IB = indices;
        commands[i].Geometry.AltGeom.IndexType = IndexType::Uint16;
        commands[i].Bindings.Buffers = {&overrides[i], 1};
        commands[i].IndexCount = 3;
        commands[i].FirstIndex = 3;
    }
    commands[0].Material = &m_Material;
    if (worldPass)
        commands[1].Material = &m_Material;
    // The materialless contributor's primary range is degenerate. Its sub-draw covers the target.
    commands[1].FirstIndex = 0;
    const DrawSubCommand sub{3, 1, 3, 0, 0};
    commands[1].SubDraws = {&sub, 1};

    RG::RGResourcePool persistent(m_Device.get());
    RG::RGTransientPool transient(m_Device.get());
    RG::RGUploadRing uploads(m_Device.get(), 2, 4096);
    RG::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
    frame.BeginFrame(0);
    m_Services.BeginWorldDrawFrame();
    const auto camera = m_Services.Views().AllocateCamera("PreparedContributorCamera");
    const auto view = m_Services.Views().AllocateView("PreparedContributorView", camera);
    TextureDesc desc{};
    desc.width = desc.height = 4;
    desc.format = static_cast<uint32_t>(format.ColorFormats[0]);
    desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    const auto target = frame.CreateTexture("PreparedContributor.Target", desc);
    bool readyAtFirstRecord = false;
    frame.AddPass("PreparedContributor.Clear", 0,
                  [&](RG::RGPassBuilder& pass) {
                      pass.AttachColor(0, target, {.Load = RG::RGLoadOp::Clear, .Clear = {.Color = {1, 0, 0, 1}}});
                  },
                  [&](RG::RGContext&) {
                      readyAtFirstRecord = m_Device->TryGetWarmGraphicsPipeline(id, format).IsValid();
                      // The packet must already own a descriptor for the green slice.
                      overrides[1].Offset = 0;
                  });
    RenderServices::WorldPassTargetsRG targets{};
    targets.Color = target;
    if (worldPass)
    {
        m_Services.BuildWorldBatchKeys();
        for (const auto& command : commands)
            m_Services.EmitForwardCommand(view, command, ForwardDrawDepth::ColourPass);
        ASSERT_TRUE(m_Services.AddWorldPassForView(frame, view, targets, MaterialKeyword::None).Pass.IsValid());
    }
    else
        ASSERT_TRUE(m_Services.AddForwardCommandPassForView(frame, view, targets, MaterialKeyword::None, commands).IsValid());
    const auto ticket = RequestTextureReadbackRG(m_Device.get(), frame, target, "PreparedContributor.Readback");
    ASSERT_TRUE(ticket);
    const auto count = frame.SubmittedFrameCount();
    frame.Execute();
    NotifyRenderGraphExecution(frame, count, nullptr);
    m_Device->Present();
    m_Device->WaitForIdle();
    EXPECT_TRUE(readyAtFirstRecord);
    ViewReadbackResult result{};
    ASSERT_TRUE(ticket->TryGet(result));
    ASSERT_EQ(result.pixels.size(), 64u);
    for (size_t i = 0; i < result.pixels.size(); i += 4)
    {
        EXPECT_EQ(result.pixels[i], 0u);
        EXPECT_EQ(result.pixels[i + 1], 255u);
        EXPECT_EQ(result.pixels[i + 2], 0u);
        EXPECT_EQ(result.pixels[i + 3], 255u);
    }
    m_Device->DestroyBuffer(indices);
    m_Device->DestroyBuffer(uniform);
}

void MaterialBindingPlanTests::CheckDepthCommandPreparation()
{
    namespace RG = GameEngine::Rendering::RenderGraph;
    const auto vertex = Utils::LoadShaderFile("Shaders/fullscreen_noinput.vert.spv");
    ShaderProgramCompileRequest request{};
    request.debugName = "PreparedDepthBindings";
    request.baseDirectory = m_Cache.Path();
    request.cacheRoot = m_Cache.Path() / "ShaderCache";
    ShaderStageCompileSpec fragmentStage{};
    fragmentStage.stage = "fs";
    fragmentStage.sourcePath = m_Cache.Path() / "prepared_depth.frag";
    fragmentStage.inlineSource = R"glsl(#version 450
layout(set = 0, binding = 5) uniform CameraUBO { vec4 sampleValue; } cam;
void main() { gl_FragDepth = cam.sampleValue.x; }
)glsl";
    request.stages.push_back(std::move(fragmentStage));
    ShaderProgramCompileResult compiled{};
    std::string compileError;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        request, ShaderSourceKind::SpirV, compiled, &compileError)) << compileError;
    ASSERT_TRUE(compiled.stageBytes.contains("fs"));
    const auto& fragment = compiled.stageBytes.at("fs");
    ASSERT_FALSE(vertex.empty());
    ASSERT_FALSE(fragment.empty());
    StageReflectionResult stage;
    std::string error;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fragment.data()),
                             fragment.size() / sizeof(uint32_t), {}, stage, &error)) << error;
    const auto meta = MergeStages({stage});
    ASSERT_EQ(meta.Sets.size(), 1u);
    ASSERT_EQ(meta.Sets.front().Bindings.size(), 1u);
    GraphicsPipelineDesc pipeline{};
    pipeline.VertexShader = std::make_shared<const std::vector<uint8_t>>(vertex);
    pipeline.PixelShader = std::make_shared<const std::vector<uint8_t>>(fragment);
    pipeline.Rasterization.cullMode = GameEngine::Rendering::CullModeFlagBits::None;
    pipeline.DepthStencil.depthTestEnable = true;
    pipeline.DepthStencil.depthWriteEnable = true;
    pipeline.DepthStencil.depthCompareOp = CompareOp::Always;
    for (const auto& layout : MaterialBuilder::BuildSetLayouts(meta))
        pipeline.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    const auto id = m_Device->InternGraphicsPipeline(pipeline);
    ASSERT_TRUE(id.IsValid());
    PipelineFormatKey format{};
    format.DepthFormat = TextureFormat::D32_FLOAT;
    ASSERT_FALSE(m_Device->TryGetWarmGraphicsPipeline(id, format).IsValid());

    ASSERT_TRUE(m_Device->BeginFrame());
    const auto uniform = MakeTestBuffer(*m_Device, 512, BufferUsage::Uniform);
    const std::array<uint16_t, 6> triangles{0, 0, 0, 0, 1, 2};
    const auto indices = MakeTestBuffer(*m_Device, sizeof(triangles), BufferUsage::Index);
    ASSERT_TRUE(uniform.IsValid() && indices.IsValid());
    constexpr float kBlue = 0.25f, kGreen = 0.75f;
    m_Device->UpdateBuffer(uniform, 0, sizeof(kBlue), &kBlue);
    m_Device->UpdateBuffer(uniform, 256, sizeof(kGreen), &kGreen);
    m_Device->UpdateBuffer(indices, 0, sizeof(triangles), triangles.data());
    const auto name = meta.Sets.front().Bindings.front().ResolvedNameId();
    std::array<DrawBindings::BufferEntry, 2> overrides{{{name, uniform, 0, 208}, {name, uniform, 256, 208}}};
    std::array<DrawCommand, 2> commands{};
    for (size_t i = 0; i < commands.size(); ++i)
    {
        commands[i].InternedPipeline = id;
        commands[i].PipelineMeta = &meta;
        commands[i].Geometry.AltGeom.IB = indices;
        commands[i].Geometry.AltGeom.IndexType = IndexType::Uint16;
        commands[i].Bindings.Buffers = {&overrides[i], 1};
        commands[i].IndexCount = 3;
        commands[i].FirstIndex = 3;
    }
    commands[0].Material = &m_Material;
    // The materialless contributor's primary range is degenerate. Its sub-draw covers the target.
    commands[1].FirstIndex = 0;
    const DrawSubCommand sub{3, 1, 3, 0, 0};
    commands[1].SubDraws = {&sub, 1};

    RG::RGResourcePool persistent(m_Device.get());
    RG::RGTransientPool transient(m_Device.get());
    RG::RGUploadRing uploads(m_Device.get(), 2, 4096);
    RG::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
    frame.BeginFrame(0);
    m_Services.BeginWorldDrawFrame();
    const auto camera = m_Services.Views().AllocateCamera("PreparedDepthContributorCamera");
    const auto view = m_Services.Views().AllocateView("PreparedDepthContributorView", camera);
    TextureDesc desc{};
    desc.width = desc.height = 4;
    desc.format = static_cast<uint32_t>(format.DepthFormat);
    desc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource | TextureUsage::TransferSrc);
    const auto target = frame.CreateTexture("PreparedDepthContributor.Target", desc);
    bool readyAtFirstRecord = false;
    frame.AddPass("PreparedDepthContributor.Clear", 0,
                  [&](RG::RGPassBuilder& pass) {
                      pass.PreventCulling();
                      pass.AttachDepth(target, {.Load = RG::RGLoadOp::Clear, .Clear = {.Depth = 0.0f}});
                  },
                  [&](RG::RGContext&) {
                      readyAtFirstRecord = m_Device->TryGetWarmGraphicsPipeline(id, format).IsValid();
                      // The packet must already own a descriptor for the green slice.
                      overrides[1].Offset = 0;
                  });
    m_Services.BuildWorldBatchKeys();
    for (const auto& command : commands)
        m_Services.EmitDepthCommand(view, DepthPassType::Prepass, command);
    ASSERT_TRUE(m_Services.AddWorldDepthPrepassForView(frame, view, target, 0.0f).IsValid());
    const auto ticket = RequestTextureReadbackRG(m_Device.get(), frame, target, "PreparedDepthContributor.Readback");
    ASSERT_TRUE(ticket);
    const auto count = frame.SubmittedFrameCount();
    frame.Execute();
    NotifyRenderGraphExecution(frame, count, nullptr);
    m_Device->Present();
    m_Device->WaitForIdle();
    EXPECT_TRUE(readyAtFirstRecord);
    ViewReadbackResult result{};
    ASSERT_TRUE(ticket->TryGet(result));
    ASSERT_EQ(result.format, TextureFormat::D32_FLOAT);
    ASSERT_EQ(result.pixels.size(), 64u);
    for (size_t i = 0; i < result.pixels.size(); i += sizeof(float))
    {
        float depth = 0.0f;
        std::memcpy(&depth, result.pixels.data() + i, sizeof(depth));
        EXPECT_FLOAT_EQ(depth, kGreen);
    }
    m_Device->DestroyBuffer(indices);
    m_Device->DestroyBuffer(uniform);
}

TEST_F(MaterialBindingPlanTests, ForwardCommandsPrepareBeforeFirstRecord)
{
    CheckForwardCommandPreparation();
}

TEST_F(MaterialBindingPlanTests, WorldCommandsPrepareBeforeFirstRecord)
{
    CheckForwardCommandPreparation(true);
}

TEST_F(MaterialBindingPlanTests, DepthCommandsPrepareBeforeFirstRecord)
{
    CheckDepthCommandPreparation();
}

TEST_F(MaterialBindingPlanTests, ComputeReadsTheNewBufferSlice)
{
    CheckBufferReadback();
}

#if defined(__APPLE__)
class MetalMaterialBindingPlanTests : public MaterialBindingPlanTests
{
  protected:
    GraphicsAPI RequestedAPI() const override { return GraphicsAPI::Metal; }
};

TEST_F(MetalMaterialBindingPlanTests, ForwardCommandsPrepareBeforeFirstRecord)
{
    CheckForwardCommandPreparation();
}

TEST_F(MetalMaterialBindingPlanTests, WorldCommandsPrepareBeforeFirstRecord)
{
    CheckForwardCommandPreparation(true);
}

TEST_F(MetalMaterialBindingPlanTests, DepthCommandsPrepareBeforeFirstRecord)
{
    CheckDepthCommandPreparation();
}

TEST_F(MetalMaterialBindingPlanTests, ComputeReadsTheNewBufferSlice)
{
    CheckBufferReadback();
}
#endif

namespace
{
class ForwardSpanDeviceFrame
{
  public:
    explicit ForwardSpanDeviceFrame(IDevice& device)
        : m_Device(device), m_Open(device.BeginFrame()) {}
    ~ForwardSpanDeviceFrame() { Finish(); }
    bool IsOpen() const { return m_Open; }
    void Finish()
    {
        if (!m_Open)
            return;
        m_Device.FinalizeFrame();
        m_Device.WaitForIdle();
        m_Open = false;
    }

  private:
    IDevice& m_Device;
    bool m_Open = false;
};

struct ForwardSpanSources
{
    ColorLinear Color{0.0f, 1.0f, 0.0f, 1.0f};
    DrawSubCommand SubDraw{3u, 1u, 3u, 0u, -1};
    DrawBindings::BufferEntry Buffer;

    void Mutate()
    {
        Color = ColorLinear{0.0f, 0.0f, 1.0f, 1.0f};
        SubDraw.FirstIndex = 0u;
        Buffer.Offset = 0u;
    }
};

void ExpectForwardSpanTarget(const ViewReadbackResult& result,
                             const std::array<uint8_t, 4>& expected, const char* name)
{
    SCOPED_TRACE(name);
    ASSERT_EQ(result.format, TextureFormat::RGBA8_UNORM);
    ASSERT_EQ(result.pixels.size(), 64u);
    for (size_t i = 0; i < result.pixels.size(); i += 4u)
    {
        for (size_t channel = 0; channel < expected.size(); ++channel)
            EXPECT_EQ(result.pixels[i + channel], expected[channel])
                << "pixel " << i / 4u << ", channel " << channel;
    }
}
} // namespace

void MaterialBindingPlanTests::CheckForwardSpanOwnership(bool worldPass, bool afterDeclaration)
{
    namespace RG = GameEngine::Rendering::RenderGraph;
    const auto vertex = Utils::LoadShaderFile("Shaders/fullscreen_noinput.vert.spv");
    const auto uniformFragment = Utils::LoadShaderFile("Shaders/ubo_sample.frag.spv");
    ASSERT_FALSE(vertex.empty());
    ASSERT_FALSE(uniformFragment.empty());

    ShaderProgramCompileRequest request{};
    request.debugName = "ForwardSpanOwnership";
    request.baseDirectory = m_Cache.Path();
    request.cacheRoot = m_Cache.Path() / "ShaderCache";
    ShaderStageCompileSpec fragmentStage{};
    fragmentStage.stage = "fs";
    fragmentStage.sourcePath = m_Cache.Path() / "forward_span.frag";
    fragmentStage.inlineSource = R"glsl(#version 450
layout(push_constant) uniform SpanColor { vec4 color; } pc;
layout(location = 0) out vec4 outColor;
void main() { outColor = pc.color; }
)glsl";
    request.stages.push_back(std::move(fragmentStage));
    ShaderProgramCompileResult compiled{};
    std::string error;
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        request, ShaderSourceKind::SpirV, compiled, &error)) << error;
    ASSERT_TRUE(compiled.stageBytes.contains("fs"));
    const auto& colorFragment = compiled.stageBytes.at("fs");
    ASSERT_FALSE(colorFragment.empty());

    StageReflectionResult colorStage, uniformStage;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment,
        reinterpret_cast<const uint32_t*>(colorFragment.data()), colorFragment.size() / sizeof(uint32_t),
        {}, colorStage, &error)) << error;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment,
        reinterpret_cast<const uint32_t*>(uniformFragment.data()), uniformFragment.size() / sizeof(uint32_t),
        {}, uniformStage, &error)) << error;
    const auto colorMeta = MergeStages({colorStage});
    const auto uniformMeta = MergeStages({uniformStage});
    ASSERT_TRUE(colorMeta.Sets.empty());
    ASSERT_EQ(uniformMeta.Sets.size(), 1u);
    ASSERT_EQ(uniformMeta.Sets.front().Bindings.size(), 1u);

    GraphicsPipelineDesc colorPipeline{};
    colorPipeline.VertexShader = std::make_shared<const std::vector<uint8_t>>(vertex);
    colorPipeline.PixelShader = std::make_shared<const std::vector<uint8_t>>(colorFragment);
    colorPipeline.Rasterization.cullMode = CullModeFlagBits::None;
    colorPipeline.ColorBlend.attachments.emplace_back();
    static_assert(sizeof(ColorLinear) == 16u);
    colorPipeline.PushConstants.Size = sizeof(ColorLinear);
    colorPipeline.PushConstants.StageMask = kShaderStageFragment;
    const auto colorId = m_Device->InternGraphicsPipeline(colorPipeline);
    ASSERT_TRUE(colorId.IsValid());

    GraphicsPipelineDesc uniformPipeline{};
    uniformPipeline.VertexShader = colorPipeline.VertexShader;
    uniformPipeline.PixelShader = std::make_shared<const std::vector<uint8_t>>(uniformFragment);
    uniformPipeline.Rasterization.cullMode = CullModeFlagBits::None;
    uniformPipeline.ColorBlend.attachments.emplace_back();
    for (const auto& layout : MaterialBuilder::BuildSetLayouts(uniformMeta))
        uniformPipeline.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    const auto uniformId = m_Device->InternGraphicsPipeline(uniformPipeline);
    ASSERT_TRUE(uniformId.IsValid());

    ForwardSpanDeviceFrame deviceFrame(*m_Device);
    ASSERT_TRUE(deviceFrame.IsOpen());
    const std::array<uint16_t, 6> triangles{1, 1, 1, 1, 2, 3};
    const auto indices = MakeTestBuffer(*m_Device, sizeof(triangles), BufferUsage::Index);
    const auto uniform = MakeTestBuffer(*m_Device, 512, BufferUsage::Uniform);
    ASSERT_TRUE(indices.IsValid() && uniform.IsValid());
    m_Device->UpdateBuffer(indices, 0, sizeof(triangles), triangles.data());
    constexpr float kBlue = 0.0f, kGreen = 1.0f;
    m_Device->UpdateBuffer(uniform, 0, sizeof(kBlue), &kBlue);
    m_Device->UpdateBuffer(uniform, 256, sizeof(kGreen), &kGreen);

    ForwardSpanSources source;
    source.Buffer = {uniformMeta.Sets.front().Bindings.front().ResolvedNameId(), uniform, 256, 208};
    const ColorLinear subDrawColor{0.0f, 1.0f, 0.0f, 1.0f};
    std::array<DrawCommand, 3> commands{};
    for (auto& command : commands)
    {
        command.InternedPipeline = colorId;
        command.PipelineMeta = &colorMeta;
        command.Material = worldPass ? &m_Material : nullptr;
        command.Geometry.AltGeom.IB = indices;
        command.Geometry.AltGeom.IndexType = IndexType::Uint16;
        command.IndexCount = 3u;
        command.FirstIndex = 3u;
        command.VertexOffset = -1;
    }
    commands[0].Bindings.PushConstants = {
        reinterpret_cast<const std::byte*>(&source.Color), sizeof(source.Color)};
    commands[1].Bindings.PushConstants = {
        reinterpret_cast<const std::byte*>(&subDrawColor), sizeof(subDrawColor)};
    commands[1].FirstIndex = 0u;
    commands[1].SubDraws = {&source.SubDraw, 1u};
    commands[2].InternedPipeline = uniformId;
    commands[2].PipelineMeta = &uniformMeta;
    commands[2].Bindings.Buffers = {&source.Buffer, 1u};

    RG::RGResourcePool persistent(m_Device.get());
    RG::RGTransientPool transient(m_Device.get());
    RG::RGUploadRing uploads(m_Device.get(), 2, 4096);
    RG::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
    frame.BeginFrame(41);
    m_Services.BeginWorldDrawFrame();
    const auto camera = m_Services.Views().AllocateCamera("ForwardSpanCamera");
    std::array<RG::RGTexture, 4> targets{};
    std::array<GameEngine::Rendering::ViewId, 3> views{};
    constexpr std::array<const char*, 4> kNames{
        "ForwardSpan.PushConstants", "ForwardSpan.SubDraws", "ForwardSpan.BufferSlice", "ForwardSpan.Background"};
    TextureDesc desc{};
    desc.width = desc.height = 4u;
    desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    for (size_t i = 0; i < targets.size(); ++i)
        targets[i] = frame.CreateTexture(kNames[i], desc);
    for (size_t i = 0; i < views.size(); ++i)
        views[i] = m_Services.Views().AllocateView(kNames[i], camera);

    uint32_t clearRecords = 0u;
    frame.AddPass("ForwardSpan.Clear", 0,
        [&](RG::RGPassBuilder& pass) {
            for (uint32_t i = 0; i < targets.size(); ++i)
                pass.AttachColor(i, targets[i],
                    {.Load = RG::RGLoadOp::Clear, .Clear = {.Color = {1, 0, 0, 1}}});
        },
        [&](RG::RGContext&) {
            ++clearRecords;
            if (!afterDeclaration)
                source.Mutate();
        });

    if (worldPass)
        m_Services.BuildWorldBatchKeys();
    for (size_t i = 0; i < commands.size(); ++i)
    {
        const auto view = views[i];
        RenderServices::WorldPassTargetsRG passTargets{};
        passTargets.Color = targets[i];
        if (worldPass)
        {
            m_Services.EmitForwardCommand(view, commands[i], ForwardDrawDepth::ColourPass);
            ASSERT_TRUE(m_Services.AddWorldPassForView(
                frame, view, passTargets, MaterialKeyword::None).Pass.IsValid());
        }
        else
            ASSERT_TRUE(m_Services.AddForwardCommandPassForView(
                frame, view, passTargets, MaterialKeyword::None, {&commands[i], 1u}).IsValid());
    }
    const auto pushReadback = RequestTextureReadbackRG(m_Device.get(), frame, targets[0], "ForwardSpan.ReadPush");
    const auto subReadback = RequestTextureReadbackRG(m_Device.get(), frame, targets[1], "ForwardSpan.ReadSub");
    const auto bufferReadback = RequestTextureReadbackRG(m_Device.get(), frame, targets[2], "ForwardSpan.ReadBuffer");
    const auto backgroundReadback = RequestTextureReadbackRG(m_Device.get(), frame, targets[3], "ForwardSpan.ReadBackground");
    ASSERT_TRUE(pushReadback && subReadback && bufferReadback && backgroundReadback);

    bool allowRecording = false;
    uint32_t prepareAttempts = 0u;
    frame.AddPass("ForwardSpan.Abort", std::numeric_limits<int32_t>::max(),
        [&](RG::RGPassBuilder& pass) {
            pass.PreventCulling();
            pass.Prepare([&](RG::RGPrepareContext&) {
                ++prepareAttempts;
                return allowRecording;
            });
        }, [](RG::RGContext&) {});
    if (afterDeclaration)
        source.Mutate();
    for (uint32_t attempt = 1u; attempt <= 2u; ++attempt)
    {
        frame.Execute();
        EXPECT_EQ(prepareAttempts, attempt);
        EXPECT_EQ(clearRecords, 0u);
        EXPECT_EQ(frame.SubmittedFrameCount(), 0u);
    }
    allowRecording = true;
    const auto submitted = frame.SubmittedFrameCount();
    frame.Execute();
    NotifyRenderGraphExecution(frame, submitted, nullptr);
    deviceFrame.Finish();
    EXPECT_EQ(prepareAttempts, 3u);
    EXPECT_EQ(clearRecords, 1u);
    EXPECT_EQ(frame.SubmittedFrameCount(), 1u);
    ViewReadbackResult pushResult{}, subResult{}, bufferResult{}, backgroundResult{};
    ASSERT_TRUE(pushReadback->TryGet(pushResult));
    ASSERT_TRUE(subReadback->TryGet(subResult));
    ASSERT_TRUE(bufferReadback->TryGet(bufferResult));
    ASSERT_TRUE(backgroundReadback->TryGet(backgroundResult));
    constexpr std::array<uint8_t, 4> kGreenPixel{0u, 255u, 0u, 255u};
    constexpr std::array<uint8_t, 4> kRedPixel{255u, 0u, 0u, 255u};
    ExpectForwardSpanTarget(pushResult, kGreenPixel, "push constants");
    ExpectForwardSpanTarget(subResult, kGreenPixel, "sub-draw index range");
    ExpectForwardSpanTarget(bufferResult, kGreenPixel, "buffer binding slice");
    ExpectForwardSpanTarget(backgroundResult, kRedPixel, "cleared background control");
    m_Device->DestroyBuffer(uniform);
    m_Device->DestroyBuffer(indices);
}

TEST_F(MaterialBindingPlanTests, ForwardCommandSpansSurviveProducerReuseAfterDeclaration)
{
    CheckForwardSpanOwnership(false, true);
}
TEST_F(MaterialBindingPlanTests, ForwardCommandSpansStayFrozenDuringRecording)
{
    CheckForwardSpanOwnership(false, false);
}
TEST_F(MaterialBindingPlanTests, WorldCommandSpansSurviveProducerReuseAfterDeclaration)
{
    CheckForwardSpanOwnership(true, true);
}
TEST_F(MaterialBindingPlanTests, WorldCommandSpansStayFrozenDuringRecording)
{
    CheckForwardSpanOwnership(true, false);
}
#if defined(__APPLE__)
TEST_F(MetalMaterialBindingPlanTests, ForwardCommandSpansSurviveProducerReuseAfterDeclaration)
{
    CheckForwardSpanOwnership(false, true);
}
TEST_F(MetalMaterialBindingPlanTests, ForwardCommandSpansStayFrozenDuringRecording)
{
    CheckForwardSpanOwnership(false, false);
}
TEST_F(MetalMaterialBindingPlanTests, WorldCommandSpansSurviveProducerReuseAfterDeclaration)
{
    CheckForwardSpanOwnership(true, true);
}
TEST_F(MetalMaterialBindingPlanTests, WorldCommandSpansStayFrozenDuringRecording)
{
    CheckForwardSpanOwnership(true, false);
}
#endif
