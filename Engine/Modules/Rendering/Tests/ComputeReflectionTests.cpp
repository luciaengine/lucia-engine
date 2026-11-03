#include <gtest/gtest.h>
#include <vector>
#include <fstream>
#include <cstring>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"

#include "TestUtils.h"

using namespace GameEngine::Rendering;

TEST(ComputeReflection, MinimalCompute_DescriptorSetsAndPC) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    // Load via TestUtils for stability across working dirs
    std::vector<uint8_t> csBytes = GameEngine::Rendering::Tests::ReadSpirvBytes("minimal_test.comp.spv");
    ASSERT_FALSE(csBytes.empty()) << "Missing minimal_test.comp.spv in Rendering/Shaders";
    ASSERT_EQ(csBytes.size() % 4, 0u);
    std::vector<uint32_t> cs(csBytes.size()/4);
    std::memcpy(cs.data(), csBytes.data(), csBytes.size());

    ReflectionOptions opts{}; StageReflectionResult rcs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, cs.data(), cs.size(), opts, rcs, &err)) << err;

    ShaderMeta meta = MergeStages({rcs});

    // Validate sets/bindings if present; ensure push constants are within policy
    for (const auto& setInfo : meta.Sets) {
        for (const auto& binding : setInfo.Bindings) {
            EXPECT_GE(binding.Binding, 0u);
            EXPECT_GE(binding.StagesMask, 0u);
        }
    }
    for (const auto& pc : meta.PushConstants) {
        EXPECT_LE(pc.Size, 128u);
    }
}


namespace {
// The engine format reflection gives the storage image at (set, binding) of the compiled compute
// shader `name`; 0 when the shader has no such storage image.
uint32_t ReflectedStorageFormat(const char* name, uint32_t set, uint32_t binding) {
    const std::vector<uint8_t> bytes = GameEngine::Rendering::Tests::ReadSpirvBytes(name);
    EXPECT_FALSE(bytes.empty()) << "Missing " << name;
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    ReflectionOptions opts{}; StageReflectionResult result{}; std::string err;
    EXPECT_TRUE(ReflectSpirv(ShaderStageKind::Compute, words.data(), words.size(), opts, result, &err)) << name << ": " << err;
    for (const auto& setMeta : MergeStages({result}).Sets)
        for (const auto& b : setMeta.Bindings)
            if (setMeta.Set == set && b.Binding == binding && b.Image)
                return b.Image->Format;
    return 0;
}
} // namespace

// A layout built from reflection binds a storage texture of the declared format: the WebGPU
// backend rejects a pipeline whose layout names another one, and the frame's encoder with it.
TEST(ComputeReflection, StorageImagesReflectTheirDeclaredFormat) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    EXPECT_EQ(ReflectedStorageFormat("gtao_prepare.comp.spv", 0, 1), static_cast<uint32_t>(TextureFormat::R32G32_FLOAT));
    EXPECT_EQ(ReflectedStorageFormat("gtao_temporal.comp.spv", 0, 5), static_cast<uint32_t>(TextureFormat::R32G32_FLOAT));
    EXPECT_EQ(ReflectedStorageFormat("msm_cascade.comp.spv", 0, 1), static_cast<uint32_t>(TextureFormat::R16G16B16A16_UNORM));
}

// A layout MaterialBuilder builds from reflection gives a `readonly image2D` read-only storage access
// and its declared texel format: WebGPU rejects a write-only entry against a read-only image.
TEST(ComputeReflection, BuiltLayoutsKeepAReadOnlyStorageImageReadOnly) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    const std::vector<uint8_t> bytes = GameEngine::Rendering::Tests::ReadSpirvBytes("gtao_depth_mip.comp.spv");
    ASSERT_FALSE(bytes.empty()) << "Missing gtao_depth_mip.comp.spv";
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    ReflectionOptions opts{}; StageReflectionResult result{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, words.data(), words.size(), opts, result, &err)) << err;

    const std::vector<DescriptorSetLayoutDesc> layouts = MaterialBuilder::BuildSetLayouts(MergeStages({result}));
    ASSERT_FALSE(layouts.empty());
    const DescriptorBinding* depthIn = nullptr;
    const DescriptorBinding* depthOut = nullptr;
    for (const DescriptorBinding& binding : layouts.front().bindings) {
        if (binding.binding == 0) depthIn = &binding;
        if (binding.binding == 1) depthOut = &binding;
    }
    ASSERT_NE(depthIn, nullptr);
    ASSERT_NE(depthOut, nullptr);
    EXPECT_TRUE(depthIn->storageReadOnly) << "uDepthIn is declared readonly";
    EXPECT_FALSE(depthOut->storageReadOnly) << "uDepthOut is declared writeonly";
    EXPECT_EQ(depthIn->storageTexelFormat, static_cast<uint32_t>(TextureFormat::R32_FLOAT));
}

TEST(ComputeReflection, AnUnmappedStorageImageFormatIsRefusedWithTheBindingNamed) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    constexpr uint32_t kOpTypeImage = 25;          // SpvOpTypeImage
    constexpr uint32_t kImageFormatOperand = 8;    // word of OpTypeImage holding the Image Format
    constexpr uint32_t kImageFormatRg32f = 6;      // SpvImageFormatRg32f
    constexpr uint32_t kImageFormatRgba8ui = 32;   // SpvImageFormatRgba8ui
    constexpr size_t kHeaderWords = 5;

    const std::vector<uint8_t> bytes = GameEngine::Rendering::Tests::ReadSpirvBytes("gtao_prepare.comp.spv");
    ASSERT_FALSE(bytes.empty()) << "Missing gtao_prepare.comp.spv";
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    int patched = 0;
    for (size_t i = kHeaderWords; i < words.size();) {
        const uint32_t wordCount = words[i] >> 16;
        if (wordCount == 0) break;
        if ((words[i] & 0xFFFFu) == kOpTypeImage && wordCount > kImageFormatOperand &&
            words[i + kImageFormatOperand] == kImageFormatRg32f) {
            words[i + kImageFormatOperand] = kImageFormatRgba8ui;
            ++patched;
        }
        i += wordCount;
    }
    ASSERT_EQ(patched, 1) << "gtao_prepare.comp.spv should declare one rg32f storage image";

    ReflectionOptions opts{}; StageReflectionResult result{}; std::string err;
    EXPECT_FALSE(ReflectSpirv(ShaderStageKind::Compute, words.data(), words.size(), opts, result, &err));
    EXPECT_NE(err.find("storage image 'uGuide' (set 0, binding 1)"), std::string::npos) << err;
    EXPECT_NE(err.find("ImageFormatToEngineFormat does not map"), std::string::npos) << err;
}
