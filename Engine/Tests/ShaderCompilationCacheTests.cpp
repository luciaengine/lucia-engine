// Tests for ShaderCompilationCache: global dedup, clear/cleanup behavior, and
// the invalidation-epoch resurrection guard.
//
// Most tests exercise the caching layer without real shader compilation.
// GetOrCompile will fail to compile (no composed shader directory) and cache
// nullptr — the caching behavior is identical for nullptr and real variants.
// ShaderCompilationCacheEpochTest is the exception: it compiles real SPIR-V from
// the staged adapters (no device needed) because the epoch guard's whole point is
// which BYTES end up cached.

#include "Engine/Rendering/ShaderCompilationCache.h"

#include "AssetCore/GUID.h"
#include "Engine/Rendering/ShaderCompileErrorLog.h"
#include "Rendering/Core/Device.h"
#include "ScopedShaderCompileTarget.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <spirv/unified1/spirv.hpp>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{

MaterialCompileSpec MakeTestSpec(const std::string& surfaceShader)
{
    MaterialCompileSpec spec{};
    spec.surfaceShaderPath = surfaceShader;
    spec.lightingModel = "StandardPBR";
    return spec;
}

ShaderCacheKey MakeTestKey(const std::string& surfaceShader)
{
    ShaderCacheKey key{};
    key.VariantKey.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.VariantKey.lightingModel = LightingModel::kStandardPBR;
    key.Source.SurfaceShaderPath = surfaceShader;
    return key;
}

// Did the sweep NAME this source identity? Asserting on the returned identities
// rather than on a standing "does this row contain the file" query pins the
// contract that matters: the identity list is the only thing driving the
// affected-material requeue, so a closure the sweep records but does not report
// is a material that never recompiles.
bool NamesIdentity(const ShaderFileDependents& deps, const std::string& surfaceShaderPath,
                   const std::string& vertexModifierPath,
                   const std::filesystem::path& materialAssetPath = {})
{
    const ShaderSourceKey wanted{surfaceShaderPath, vertexModifierPath, materialAssetPath};
    return std::find(deps.Identities.begin(), deps.Identities.end(), wanted)
           != deps.Identities.end();
}

} // namespace

class ShaderCompilationCacheTest : public ::testing::Test
{
  protected:
    static Rendering::MaterialBuildContext MakeFakeContext()
    {
        // Named-field init: positional aggregate init silently re-maps when the
        // context grows a field (it did — PackageShaderDirs).
        Rendering::MaterialBuildContext ctx{};
        ctx.AdapterShaderDir = "nonexistent_composed_dir";
        ctx.CacheRoot = ".Cache/Shaders";
        return ctx;
    }

    ShaderCompilationCache m_cache;
    Rendering::MaterialBuildContext m_fakeContext = MakeFakeContext();
};

// --- Global cache ---

// Regression: an empty AdapterShaderDir means the build context isn't ready yet
// (early startup, before the editor 'editor' asset source is registered). This is
// transient, not a genuine compile error — it must NOT poison the cache, or the
// material's color variant stays permanently dead for the session even after the
// context becomes available. Symptom this caused: a mesh casts shadows but is
// invisible in the color pass (the shared depth shader works; the color variant
// was cached as nullptr). A genuine failure (non-empty but invalid dir) still caches.
TEST_F(ShaderCompilationCacheTest, GetOrCompile_EmptyContext_DoesNotPoisonCache)
{
    Rendering::MaterialBuildContext emptyContext{};
    ASSERT_TRUE(emptyContext.AdapterShaderDir.empty());

    const ShaderCacheKey key = MakeTestKey("surface_a.glsl");
    const MaterialCompileSpec spec = MakeTestSpec("surface_a.glsl");

    auto first = m_cache.GetOrCompile(key, spec, emptyContext, "MatA", ShaderSourceKind::SpirV);
    EXPECT_EQ(first, nullptr);
    EXPECT_EQ(m_cache.GetEntryCount(), 0u);

    // A populated (but here still invalid) context resumes normal caching, so the
    // material recompiles once the context is ready rather than staying dead.
    m_cache.GetOrCompile(key, spec, m_fakeContext, "MatA", ShaderSourceKind::SpirV);
    EXPECT_EQ(m_cache.GetEntryCount(), 1u);
}

TEST_F(ShaderCompilationCacheTest, GetOrCompile_CacheHitReturnsSameResult)
{
    const ShaderCacheKey key = MakeTestKey("surface_a.glsl");
    const MaterialCompileSpec spec = MakeTestSpec("surface_a.glsl");

    auto first = m_cache.GetOrCompile(key, spec, m_fakeContext, "MatA", ShaderSourceKind::SpirV);
    auto second = m_cache.GetOrCompile(key, spec, m_fakeContext, "MatA", ShaderSourceKind::SpirV);

    EXPECT_EQ(first, second);
    EXPECT_EQ(m_cache.GetEntryCount(), 1u);
}

// --- Clear ---

TEST_F(ShaderCompilationCacheTest, Clear_RemovesGlobalEntries)
{
    const MaterialCompileSpec spec = MakeTestSpec("surface_a.glsl");
    m_cache.GetOrCompile(MakeTestKey("surface_a.glsl"), spec, m_fakeContext, "MatA",
                         ShaderSourceKind::SpirV);
    ShaderCacheKey forwardPlusKey = MakeTestKey("surface_a.glsl");
    forwardPlusKey.VariantKey.materialKeywords |= MaterialKeyword::ForwardPlus;
    m_cache.GetOrCompile(forwardPlusKey, spec, m_fakeContext, "MatA", ShaderSourceKind::SpirV);
    ASSERT_EQ(m_cache.GetEntryCount(), 2u);

    m_cache.Clear();

    EXPECT_EQ(m_cache.GetEntryCount(), 0u);
}

// --- Re-population after clear ---

TEST_F(ShaderCompilationCacheTest, GetOrCompile_WorksAfterClear)
{
    const ShaderCacheKey key = MakeTestKey("surface_a.glsl");
    const MaterialCompileSpec spec = MakeTestSpec("surface_a.glsl");

    m_cache.GetOrCompile(key, spec, m_fakeContext, "MatA", ShaderSourceKind::SpirV);
    m_cache.Clear();

    m_cache.GetOrCompile(key, spec, m_fakeContext, "MatA", ShaderSourceKind::SpirV);
    EXPECT_EQ(m_cache.GetEntryCount(), 1u);
}

TEST_F(ShaderCompilationCacheTest, InvalidateGlobalEntry_ForcesGlobalRecompile)
{
    const ShaderCacheKey key = MakeTestKey("surface_a.glsl");
    const MaterialCompileSpec spec = MakeTestSpec("surface_a.glsl");

    m_cache.GetOrCompile(key, spec, m_fakeContext, "MatA", ShaderSourceKind::SpirV);
    EXPECT_EQ(m_cache.GetEntryCount(), 1u);

    m_cache.InvalidateGlobalEntry(key);
    EXPECT_EQ(m_cache.GetEntryCount(), 0u);

    m_cache.GetOrCompile(key, spec, m_fakeContext, "MatA", ShaderSourceKind::SpirV);
    EXPECT_EQ(m_cache.GetEntryCount(), 1u);
}

// --- Invalidation-epoch guard: the in-flight-compile resurrection race ---
//
// A compile already in flight when an edit notification lands must NOT publish
// its PRE-EDIT SPIR-V into the caches after InvalidateEntriesForShaderFile swept
// them. Left unguarded that leaves half-new/half-old variants on screen until the
// next save — and rapid save bursts, the core authoring workload, are exactly
// when in-flight compiles overlap edits.
//
// These tests compile real SPIR-V against the staged adapter shaders (shaderc
// only; no Vulkan device is involved in GetOrCompile) and assert on the compiled
// BYTES: a guard that merely dropped a pointer would not satisfy them.
// SetCompileEnteredHookForTesting runs the invalidation on the compiling thread
// strictly between the publish's epoch snapshot and its insert — the one
// interleaving no external thread can hit deterministically.
class ShaderCompilationCacheEpochTest : public ::testing::Test
{
  protected:
    static constexpr const char* kSurfaceFile = "epoch_surface.glsl";

    void SetUp() override
    {
        m_shaderDir = GameEngine::TestPaths::StagedRenderingShadersDir();
        std::error_code ec;
        if (!std::filesystem::exists(m_shaderDir, ec))
            GTEST_SKIP() << "Staged shader directory not found: " << m_shaderDir.string();

        // Per-TEST project root AND cache root, unique to this process and this
        // test: the on-disk shaderpkg key folds the composed source, which
        // carries the surface's absolute path, so a shared root makes sibling
        // tests share cache entries.
        m_projectRoot = TestUtils::MakeUniqueTempDirectory("ge_shader_epoch");
        std::filesystem::create_directories(m_projectRoot);

        m_ctx.AdapterShaderDir = m_shaderDir;
        m_ctx.IncludeDirs = {m_shaderDir};
        m_ctx.ProjectRoots = {m_projectRoot};
        m_ctx.CacheRoot = m_projectRoot / ".Cache" / "Shaders";
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_projectRoot, ec);
    }

    // Rewrites the surface so the next compile produces different SPIR-V.
    //
    // No timestamp handling needed: the disk shaderpkg key hashes the CONTENT of
    // the surface the composer substitutes as a literal #include, so two
    // same-length bodies written inside one filesystem timestamp tick still key
    // differently. This used to need an explicit mtime push forward to dodge an
    // `includeMtime <= packageMtime` staleness shortcut that otherwise made the
    // probe serve the OLD package — observed once as a flake here.
    void WriteSurface(const char* colorExpr) const
    {
        const auto path = m_projectRoot / kSurfaceFile;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "could not open " << path.string();
        out << "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
               "    SurfaceOutput o = DefaultSurfaceOutput();\n"
               "    o.baseColor = "
            << colorExpr
            << ";\n"
               "    return o;\n"
               "}\n";
    }

    static MaterialCompileSpec MakeSpec()
    {
        MaterialCompileSpec spec{};
        spec.surfaceShaderPath = kSurfaceFile;
        spec.lightingModel = "StandardPBR";
        return spec;
    }

    static ShaderCacheKey MakeKey()
    {
        ShaderCacheKey key{};
        key.VariantKey.vertexFlags = VertexAttributeFlags::StandardMesh;
        key.VariantKey.lightingModel = LightingModel::kStandardPBR;
        key.Source.SurfaceShaderPath = kSurfaceFile;
        return key;
    }

    // Arms the seam so the NEXT compile takes a real edit notification for
    // kSurfaceFile inside its publish window. Returns the call counter so a test
    // can prove the window was actually entered (an unfired hook would make the
    // assertions below vacuous).
    std::shared_ptr<int> ArmInvalidationDuringCompile()
    {
        auto calls = std::make_shared<int>(0);
        m_cache.SetCompileEnteredHookForTesting(
            [this, calls]
            {
                ++*calls;
                m_cache.InvalidateEntriesForShaderFile(kSurfaceFile);
            });
        return calls;
    }

    void Disarm() { m_cache.SetCompileEnteredHookForTesting({}); }

    std::filesystem::path m_shaderDir;
    std::filesystem::path m_projectRoot;
    Rendering::MaterialBuildContext m_ctx;
    ShaderCompilationCache m_cache;
};

TEST_F(ShaderCompilationCacheEpochTest, InvalidateDuringCompile_DoesNotResurrectGlobalEntry)
{
    WriteSurface("vec3(0.0, 1.0, 0.0)"); // v1
    const ShaderCacheKey key = MakeKey();
    const MaterialCompileSpec spec = MakeSpec();

    auto calls = ArmInvalidationDuringCompile();
    auto stale = m_cache.GetOrCompile(key, spec, m_ctx, "EpochProbe", ShaderSourceKind::SpirV);
    Disarm();

    ASSERT_EQ(*calls, 1) << "the compile window must have been entered, or nothing was tested";
    ASSERT_NE(stale, nullptr) << "the interrupted compile still returns its result to its caller";
    ASSERT_FALSE(stale->fragmentBytes.empty()) << "the staged adapters must produce real SPIR-V";
    EXPECT_EQ(m_cache.GetEntryCount(), 0u)
        << "a publish whose compile started before the invalidation must not reinstate the "
           "swept entry";

    // The recompile after the invalidation must see the EDITED source. Without
    // the guard this is a cache HIT on the resurrected pre-edit entry.
    WriteSurface("vec3(1.0, 0.0, 0.0) * 0.25"); // v2
    auto fresh = m_cache.GetOrCompile(key, spec, m_ctx, "EpochProbe", ShaderSourceKind::SpirV);
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(m_cache.GetEntryCount(), 1u) << "an uninterrupted publish still caches";
    EXPECT_NE(fresh->fragmentBytes, stale->fragmentBytes)
        << "the post-invalidation compile must produce the edited SPIR-V, not the pre-edit bytes";
}

// InvalidateGlobalEntry must bar resurrection exactly like the file sweep: the
// entry a compile in flight is about to publish is not resident yet, so the
// zero-drop invalidation must still bump the epoch or the publish reinstates
// SPIR-V built from the pre-edit source. (The async material recompile path
// invalidates through THIS entry point, not the file sweep.)
TEST_F(ShaderCompilationCacheEpochTest, TargetedInvalidateDuringCompile_DoesNotResurrectGlobalEntry)
{
    WriteSurface("vec3(0.0, 1.0, 0.0)"); // v1
    const ShaderCacheKey key = MakeKey();
    const MaterialCompileSpec spec = MakeSpec();

    auto calls = std::make_shared<int>(0);
    m_cache.SetCompileEnteredHookForTesting(
        [this, calls, key]
        {
            ++*calls;
            m_cache.InvalidateGlobalEntry(key);
        });
    auto stale = m_cache.GetOrCompile(key, spec, m_ctx, "EpochProbe", ShaderSourceKind::SpirV);
    Disarm();

    ASSERT_EQ(*calls, 1) << "the compile window must have been entered, or nothing was tested";
    ASSERT_NE(stale, nullptr) << "the interrupted compile still returns its result to its caller";
    EXPECT_EQ(m_cache.GetEntryCount(), 0u)
        << "a targeted invalidation during the compile must bar the publish from "
           "reinstating the swept key";

    WriteSurface("vec3(1.0, 0.0, 0.0) * 0.25"); // v2
    auto fresh = m_cache.GetOrCompile(key, spec, m_ctx, "EpochProbe", ShaderSourceKind::SpirV);
    ASSERT_NE(fresh, nullptr);
    EXPECT_NE(fresh->fragmentBytes, stale->fragmentBytes)
        << "the post-invalidation compile must produce the edited SPIR-V, not the pre-edit bytes";
}

// Clear() must bar the same resurrection the sweep bars. It runs at project
// switch with prewarm workers still compiling — nothing drains them first — so a
// compile in flight across the Clear() would otherwise republish the OLD
// project's SPIR-V into the freshly emptied cache and serve it for the rest of
// the session. The row that compile recorded does come back (rows are
// deliberately not epoch-gated, or the affected-material scan could not outlive
// a sweep); the ENTRY is what must not.
TEST_F(ShaderCompilationCacheEpochTest, ClearDuringCompile_DoesNotResurrectEntry)
{
    WriteSurface("vec3(0.0, 1.0, 0.0)");
    const ShaderCacheKey key = MakeKey();
    const MaterialCompileSpec spec = MakeSpec();

    auto calls = std::make_shared<int>(0);
    m_cache.SetCompileEnteredHookForTesting(
        [this, calls]
        {
            ++*calls;
            m_cache.Clear();
        });
    auto stale = m_cache.GetOrCompile(key, spec, m_ctx, "ClearProbe", ShaderSourceKind::SpirV);
    Disarm();

    ASSERT_EQ(*calls, 1) << "the compile window must have been entered, or nothing was tested";
    ASSERT_NE(stale, nullptr) << "the interrupted compile still returns its result to its caller";
    ASSERT_FALSE(stale->fragmentBytes.empty()) << "the staged adapters must produce real SPIR-V";
    EXPECT_EQ(m_cache.GetEntryCount(), 0u)
        << "a publish whose compile started before Clear() must not reinstate the entry";

    // ...and the cache is not wedged: the next request compiles and caches.
    auto fresh = m_cache.GetOrCompile(key, spec, m_ctx, "ClearProbe", ShaderSourceKind::SpirV);
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(m_cache.GetEntryCount(), 1u);
}

// The guard must not cost a normal compile its cache entry: an invalidation for
// an UNRELATED file leaves the epoch moved for in-flight compiles, but a compile
// that starts afterwards snapshots the new epoch and publishes normally.
TEST_F(ShaderCompilationCacheEpochTest, InvalidateBeforeCompile_StillCaches)
{
    WriteSurface("vec3(0.0, 1.0, 0.0)");
    m_cache.InvalidateEntriesForShaderFile("some_other_surface.glsl");

    auto variant = m_cache.GetOrCompile(MakeKey(), MakeSpec(), m_ctx, "EpochProbe", ShaderSourceKind::SpirV);
    ASSERT_NE(variant, nullptr);
    EXPECT_EQ(m_cache.GetEntryCount(), 1u)
        << "an invalidation that predates the compile must not suppress its publish";
}

// --- Include-closure invalidation ---
//
// Entries are keyed by the AUTHORED surface / vertex-modifier references, but a
// compile reads far more: adapters, engine includes, anything the surface
// #includes transitively. Each successful (or attempted) build records that
// closure per (surface, vertex-modifier) pair, and InvalidateEntriesForShaderFile
// matches the edited filename against it — so an include edit drops every rider
// instead of firing nothing (the last "save -> nothing happens" path from the
// 2026-07-25 authoring-loop walk). Real shaderc compiles, assertions on bytes.
class ShaderCompilationCacheClosureTest : public ::testing::Test
{
  protected:
    static constexpr const char* kSurfaceFile = "closure_surface.glsl";
    static constexpr const char* kHelperFile = "closure_helper.glsl";

    void SetUp() override
    {
        m_shaderDir = GameEngine::TestPaths::StagedRenderingShadersDir();
        std::error_code ec;
        if (!std::filesystem::exists(m_shaderDir, ec))
            GTEST_SKIP() << "Staged shader directory not found: " << m_shaderDir.string();

        // Per-TEST roots for the same reason as the epoch fixture: the disk key
        // folds the surface's absolute path via the composed source.
        m_projectRoot = TestUtils::MakeUniqueTempDirectory("ge_shader_closure");
        std::filesystem::create_directories(m_projectRoot);

        m_ctx.AdapterShaderDir = m_shaderDir;
        m_ctx.IncludeDirs = {m_shaderDir};
        m_ctx.ProjectRoots = {m_projectRoot};
        m_ctx.CacheRoot = m_projectRoot / ".Cache" / "Shaders";
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_projectRoot, ec);
    }

    void WriteFile(const char* file, const std::string& text) const
    {
        const auto path = m_projectRoot / file;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "could not open " << path.string();
        out << text;
    }

    // A helper include owning the base color, and a surface that consumes it.
    // The include resolves against the surface's own directory (the FileIncluder
    // and the key scanner both probe the requesting source's parent first).
    void WriteHelper(const char* colorExpr) const
    {
        WriteFile(kHelperFile,
                  std::string("vec3 ClosureHelperColor() { return ") + colorExpr + "; }\n");
    }
    void WriteIncludingSurface(const char* surfaceFile, const char* includeFile) const
    {
        WriteFile(surfaceFile,
                  std::string("#include \"") + includeFile + "\"\n"
                  "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
                  "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                  "    o.baseColor = ClosureHelperColor();\n"
                  "    return o;\n"
                  "}\n");
    }

    static MaterialCompileSpec MakeSpec(const char* surfaceFile = kSurfaceFile)
    {
        MaterialCompileSpec spec{};
        spec.surfaceShaderPath = surfaceFile;
        spec.lightingModel = "StandardPBR";
        return spec;
    }

    static ShaderCacheKey MakeKey(const char* surfaceFile = kSurfaceFile)
    {
        ShaderCacheKey key{};
        key.VariantKey.vertexFlags = VertexAttributeFlags::StandardMesh;
        key.VariantKey.lightingModel = LightingModel::kStandardPBR;
        key.Source.SurfaceShaderPath = surfaceFile;
        return key;
    }

    std::filesystem::path m_shaderDir;
    std::filesystem::path m_projectRoot;
    Rendering::MaterialBuildContext m_ctx;
    ShaderCompilationCache m_cache;
};

TEST_F(ShaderCompilationCacheClosureTest, SameDirectoryMaterialsShareCompileAndInvalidation)
{
    WriteHelper("vec3(0.0, 1.0, 0.0)");
    WriteIncludingSurface(kSurfaceFile, kHelperFile);
    WriteFile("first.material", "{}");
    WriteFile("second.material", "{}");
    auto first = MakeKey();
    first.Source.MaterialAssetPath = m_projectRoot / "first.material";
    auto second = first;
    second.Source.MaterialAssetPath = m_projectRoot / "second.material";
    EXPECT_EQ(first, second);
    EXPECT_EQ(std::hash<ShaderCacheKey>{}(first), std::hash<ShaderCacheKey>{}(second));
    size_t compiles = 0;
    m_cache.SetCompileEnteredHookForTesting([&] { ++compiles; });
    auto a = m_cache.GetOrCompile(first, MakeSpec(), m_ctx, "First", ShaderSourceKind::SpirV);
    auto b = m_cache.GetOrCompile(second, MakeSpec(), m_ctx, "Second", ShaderSourceKind::SpirV);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a, b);
    EXPECT_EQ(compiles, 1u);
    EXPECT_EQ(first.Source.MaterialAssetPath.filename(), "first.material");
    EXPECT_EQ(second.Source.MaterialAssetPath.filename(), "second.material");
    const auto deps = m_cache.InvalidateEntriesForShaderFile(kHelperFile);
    EXPECT_EQ(deps.DroppedEntries, 1u);
    EXPECT_TRUE(NamesIdentity(deps, kSurfaceFile, "", first.Source.MaterialAssetPath));
    EXPECT_TRUE(NamesIdentity(deps, kSurfaceFile, "", second.Source.MaterialAssetPath));
    WriteHelper("vec3(1.0, 0.0, 0.0)");
    auto fresh = m_cache.GetOrCompile(second, MakeSpec(), m_ctx, "Second", ShaderSourceKind::SpirV);
    ASSERT_NE(fresh, nullptr);
    EXPECT_NE(fresh->fragmentBytes, a->fragmentBytes);
    EXPECT_EQ(compiles, 2u);
}

TEST(ShaderSourceKeyTest, SyntheticSourcesRemainDistinctFromRelativeMaterialFiles)
{
    ShaderSourceKey synthetic{"surface.glsl", "", {}};
    ShaderSourceKey authored{"surface.glsl", "", "first.material"};
    EXPECT_FALSE(synthetic == authored);
    auto other = authored;
    other.MaterialAssetPath = "second.material";
    EXPECT_EQ(authored, other);
    other.MaterialAssetPath = "other/second.material";
    EXPECT_FALSE(authored == other);
}

TEST_F(ShaderCompilationCacheClosureTest, SharedSuccessClearsOriginalMaterialDiagnostic)
{
    ShaderCompileErrorLog errors;
    m_cache.SetErrorLog(&errors);
    WriteFile("first.material", "{}");
    WriteFile("second.material", "{}");
    WriteFile(kSurfaceFile, "not valid shader source");
    auto first = MakeKey();
    first.Source.MaterialAssetPath = m_projectRoot / "first.material";
    auto second = first;
    second.Source.MaterialAssetPath = m_projectRoot / "second.material";
    EXPECT_EQ(m_cache.GetOrCompile(first, MakeSpec(), m_ctx, "First", ShaderSourceKind::SpirV), nullptr);
    ASSERT_FALSE(errors.Snapshot().empty());
    EXPECT_EQ(errors.Snapshot().front().MaterialAssetPath, first.Source.MaterialAssetPath);
    WriteHelper("vec3(0.0, 1.0, 0.0)");
    WriteIncludingSurface(kSurfaceFile, kHelperFile);
    m_cache.InvalidateEntriesForShaderFile(kSurfaceFile);
    ASSERT_NE(m_cache.GetOrCompile(second, MakeSpec(), m_ctx, "Second", ShaderSourceKind::SpirV), nullptr);
    ASSERT_NE(m_cache.GetOrCompile(first, MakeSpec(), m_ctx, "First", ShaderSourceKind::SpirV), nullptr);
    EXPECT_TRUE(errors.Snapshot().empty());
    m_cache.SetErrorLog(nullptr);
}

TEST_F(ShaderCompilationCacheClosureTest, IncludeEdit_InvalidatesRidersAndRecompilesFresh)
{
    WriteHelper("vec3(0.0, 1.0, 0.0)"); // v1
    WriteIncludingSurface(kSurfaceFile, kHelperFile);

    auto v1 = m_cache.GetOrCompile(MakeKey(), MakeSpec(), m_ctx, "ClosureProbe", ShaderSourceKind::SpirV);
    ASSERT_NE(v1, nullptr);
    ASSERT_FALSE(v1->fragmentBytes.empty());

    // The edited file appears in NO entry's key — only the recorded closure
    // knows it. The sweep must both drop the entry and name the identity.
    WriteHelper("vec3(1.0, 0.0, 0.0) * 0.25"); // v2
    const auto deps = m_cache.InvalidateEntriesForShaderFile(kHelperFile);
    EXPECT_EQ(deps.DroppedEntries, 1u)
        << "an include edit must drop the entries whose closure contains it";
    EXPECT_TRUE(NamesIdentity(deps, kSurfaceFile, ""))
        << "a successful build's include closure must make its identity reportable";
    EXPECT_EQ(m_cache.GetEntryCount(), 0u);

    auto v2 = m_cache.GetOrCompile(MakeKey(), MakeSpec(), m_ctx, "ClosureProbe", ShaderSourceKind::SpirV);
    ASSERT_NE(v2, nullptr);
    EXPECT_NE(v2->fragmentBytes, v1->fragmentBytes)
        << "the recompile after an include edit must produce the edited SPIR-V";
}

// The per-material recompile path invalidates ONE key -- the material's base
// variant (RecompileMaterialPipeline -> InvalidateGlobalEntry) -- while
// PrewarmMaterialVariants puts each pass/keyword variant in m_Cache under its own
// merged key. So base-only invalidation cannot reach a pass variant, and the
// closure sweep is the ONLY mechanism that refreshes one after a pure include
// edit. Pinned because "the sweep is redundant with the per-material recompile"
// is a plausible-sounding reading that would justify deleting a load-bearing
// defense.
TEST_F(ShaderCompilationCacheClosureTest, BaseOnlyInvalidationCannotReachPassVariant)
{
    WriteHelper("vec3(0.0, 1.0, 0.0)"); // v1
    WriteIncludingSurface(kSurfaceFile, kHelperFile);

    const ShaderCacheKey baseKey = MakeKey();
    ShaderCacheKey passKey = MakeKey();
    passKey.VariantKey.materialKeywords = MaterialKeyword::Instanced;

    auto base1 = m_cache.GetOrCompile(baseKey, MakeSpec(), m_ctx, "ProbeBase", ShaderSourceKind::SpirV);
    ASSERT_NE(base1, nullptr);
    auto pass1 = m_cache.GetOrCompile(passKey, MakeSpec(), m_ctx, "ProbePass", ShaderSourceKind::SpirV);
    ASSERT_NE(pass1, nullptr);
    ASSERT_FALSE(pass1->fragmentBytes.empty());
    ASSERT_EQ(m_cache.GetEntryCount(), 2u)
        << "base and pass variant must be SEPARATE global entries";

    WriteHelper("vec3(1.0, 0.0, 0.0) * 0.25"); // v2 -- edit the include

    // Exactly what the per-material recompile does on its own: the base key only.
    m_cache.InvalidateGlobalEntry(baseKey);
    EXPECT_EQ(m_cache.GetEntryCount(), 1u)
        << "base-only invalidation drops the base and leaves the pass variant";

    auto passAfter = m_cache.GetOrCompile(passKey, MakeSpec(), m_ctx, "ProbePass", ShaderSourceKind::SpirV);
    ASSERT_NE(passAfter, nullptr);
    EXPECT_EQ(passAfter->fragmentBytes, pass1->fragmentBytes)
        << "base-only invalidation leaves the pass variant serving pre-edit bytes";

    // Only the closure sweep reaches every variant of the pair.
    auto base2 = m_cache.GetOrCompile(baseKey, MakeSpec(), m_ctx, "ProbeBase", ShaderSourceKind::SpirV);
    ASSERT_NE(base2, nullptr);
    ASSERT_EQ(m_cache.GetEntryCount(), 2u);
    EXPECT_EQ(m_cache.InvalidateEntriesForShaderFile(kHelperFile).DroppedEntries, 2u)
        << "the closure sweep must drop EVERY variant of the pair, base and pass alike";
}

TEST_F(ShaderCompilationCacheClosureTest, UnrelatedFileEdit_DropsNothing)
{
    WriteHelper("vec3(0.0, 1.0, 0.0)");
    WriteIncludingSurface(kSurfaceFile, kHelperFile);

    auto v1 = m_cache.GetOrCompile(MakeKey(), MakeSpec(), m_ctx, "ClosureProbe", ShaderSourceKind::SpirV);
    ASSERT_NE(v1, nullptr);

    const auto deps = m_cache.InvalidateEntriesForShaderFile("not_in_closure.glsl");
    EXPECT_EQ(deps.DroppedEntries, 0u)
        << "a file outside every closure must invalidate nothing";
    EXPECT_TRUE(deps.Identities.empty())
        << "...and must name no identity for the requeue to act on";
    EXPECT_EQ(m_cache.GetEntryCount(), 1u);
    EXPECT_EQ(m_cache.GetOrCompile(MakeKey(), MakeSpec(), m_ctx, "ClosureProbe", ShaderSourceKind::SpirV), v1)
        << "the surviving entry must still serve as a hit";
}

TEST_F(ShaderCompilationCacheClosureTest, AdapterEdit_InvalidatesRiders)
{
    WriteHelper("vec3(0.0, 1.0, 0.0)");
    WriteIncludingSurface(kSurfaceFile, kHelperFile);

    ASSERT_NE(m_cache.GetOrCompile(MakeKey(), MakeSpec(), m_ctx, "ClosureProbe", ShaderSourceKind::SpirV), nullptr);

    // The adapters are INLINED into the composed source (never #include
    // directives), so only the build-service dependency record can know them.
    const auto vertexDeps = m_cache.InvalidateEntriesForShaderFile("adapter_vertex.glsl");
    EXPECT_EQ(vertexDeps.DroppedEntries, 1u)
        << "a vertex-adapter edit must invalidate every material riding it";
    EXPECT_TRUE(NamesIdentity(vertexDeps, kSurfaceFile, ""));

    ASSERT_NE(m_cache.GetOrCompile(MakeKey(), MakeSpec(), m_ctx, "ClosureProbe", ShaderSourceKind::SpirV), nullptr);
    const auto fragmentDeps = m_cache.InvalidateEntriesForShaderFile("adapter_forward.glsl");
    EXPECT_EQ(fragmentDeps.DroppedEntries, 1u)
        << "a fragment-adapter edit must invalidate every material riding it";
    EXPECT_TRUE(NamesIdentity(fragmentDeps, kSurfaceFile, ""));
    EXPECT_EQ(m_cache.GetEntryCount(), 0u);
}

TEST_F(ShaderCompilationCacheClosureTest, SharedInclude_DropsEverySurfaceRidingIt)
{
    WriteHelper("vec3(0.0, 1.0, 0.0)");
    WriteIncludingSurface("closure_surface_a.glsl", kHelperFile);
    WriteIncludingSurface("closure_surface_b.glsl", kHelperFile);

    ASSERT_NE(m_cache.GetOrCompile(MakeKey("closure_surface_a.glsl"),
                                   MakeSpec("closure_surface_a.glsl"), m_ctx, "A", ShaderSourceKind::SpirV),
              nullptr);
    ASSERT_NE(m_cache.GetOrCompile(MakeKey("closure_surface_b.glsl"),
                                   MakeSpec("closure_surface_b.glsl"), m_ctx, "B", ShaderSourceKind::SpirV),
              nullptr);
    ASSERT_EQ(m_cache.GetEntryCount(), 2u);

    const auto deps = m_cache.InvalidateEntriesForShaderFile(kHelperFile);
    EXPECT_EQ(deps.DroppedEntries, 2u)
        << "every surface including the edited file must drop, not just one";
    EXPECT_TRUE(NamesIdentity(deps, "closure_surface_a.glsl", ""));
    EXPECT_TRUE(NamesIdentity(deps, "closure_surface_b.glsl", ""))
        << "both riders must be named, not just the one whose entry was found first";
    EXPECT_EQ(m_cache.GetEntryCount(), 0u);
}

// A build that fails to COMPILE must still record the files it RESOLVED: the
// author's very next action is fixing the malformed helper, and that edit must
// re-trigger the compile (union semantics — the last-good set is kept
// alongside).
//
// The helper here EXISTS and is syntactically broken, which is the shape the
// mechanism covers. An operand that resolves NOWHERE is recorded by neither the
// closure scan nor the includer, so the missing-file case is outside both this
// test and the mechanism.
TEST_F(ShaderCompilationCacheClosureTest, FailedCompile_RecordsClosureOfPresentButBrokenInclude)
{
    WriteFile(kHelperFile, "vec3 ClosureHelperColor() { return vec3(0.0, 1.0, 0.0; }\n"); // broken
    WriteIncludingSurface(kSurfaceFile, kHelperFile);

    auto broken = m_cache.GetOrCompile(MakeKey(), MakeSpec(), m_ctx, "ClosureProbe", ShaderSourceKind::SpirV);
    EXPECT_EQ(broken, nullptr) << "the malformed include must fail the compile";

    WriteHelper("vec3(0.0, 0.0, 1.0)"); // fixed
    const auto deps = m_cache.InvalidateEntriesForShaderFile(kHelperFile);
    EXPECT_TRUE(NamesIdentity(deps, kSurfaceFile, ""))
        << "a failed build must still record the includes it resolved, or the fix never requeues it";
    EXPECT_GE(deps.DroppedEntries, 1u) << "fixing the include must drop the memoized failure";
    auto fixed = m_cache.GetOrCompile(MakeKey(), MakeSpec(), m_ctx, "ClosureProbe", ShaderSourceKind::SpirV);
    ASSERT_NE(fixed, nullptr) << "the fixed include must compile on the next request";
    EXPECT_FALSE(fixed->fragmentBytes.empty());
}

// Implicit-default-surface materials (no authored surfaceShader) ride the lane:
// their closure row is keyed on the EMPTY spec strings and records the default
// surface chain the build actually read, so editing standard_surface.glsl (or
// anything it includes) invalidates them like any authored surface.
TEST_F(ShaderCompilationCacheClosureTest, ImplicitDefaultSurface_RecordsDefaultChain)
{
    ShaderCacheKey key{};
    key.VariantKey.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.VariantKey.lightingModel = LightingModel::kStandardPBR;
    MaterialCompileSpec spec{};
    spec.lightingModel = "StandardPBR";

    auto variant = m_cache.GetOrCompile(key, spec, m_ctx, "DefaultSurfaceProbe", ShaderSourceKind::SpirV);
    ASSERT_NE(variant, nullptr) << "the implicit default surface must compile from the staged tree";
    const auto deps = m_cache.InvalidateEntriesForShaderFile("standard_surface.glsl");
    EXPECT_TRUE(NamesIdentity(deps, "", ""))
        << "the default surface chain must be recorded under the empty spec strings";
    EXPECT_EQ(deps.DroppedEntries, 1u)
        << "editing the default surface must invalidate implicit-default materials";
}

// Clear() drops the compiled entries but keeps the closure ROWS. A project switch
// clears this cache while every material registered before it survives holding a
// live pipeline and is never recompiled; without its row such a material is
// reachable only by the direct filename check, which by construction cannot fire
// for an include, an adapter or a transitive helper.
TEST_F(ShaderCompilationCacheClosureTest, ClearKeepsClosureRows)
{
    WriteHelper("vec3(0.0, 1.0, 0.0)");
    WriteIncludingSurface(kSurfaceFile, kHelperFile);
    ASSERT_NE(m_cache.GetOrCompile(MakeKey(), MakeSpec(), m_ctx, "ClosureProbe", ShaderSourceKind::SpirV), nullptr);
    ASSERT_EQ(m_cache.GetEntryCount(), 1u);

    m_cache.Clear();
    ASSERT_EQ(m_cache.GetEntryCount(), 0u) << "Clear must drop the compiled entries";

    // The edited file is in no entry (there are none) and in no spec — the
    // surviving row is the only thing that can name the identity.
    WriteHelper("vec3(1.0, 0.0, 0.0) * 0.25");
    const auto deps = m_cache.InvalidateEntriesForShaderFile(kHelperFile);
    EXPECT_EQ(deps.DroppedEntries, 0u) << "nothing is left to drop after the clear";
    EXPECT_TRUE(NamesIdentity(deps, kSurfaceFile, ""))
        << "the closure row must outlive Clear(), or a material that survives a project switch "
           "can never be reached by an include edit again";
}

// Two materials in DIFFERENT directories can author the SAME surface filename and
// resolve to different files. The closure row is keyed on the full source
// identity — material asset path included — so they get SEPARATE rows and an
// edit to one's private helper leaves the other's entry alone. Keying the row on
// (surface, vertex-modifier) alone shared one row between them, which then had to
// union both helper sets to avoid a missed invalidation, and that union cost the
// co-rider a redundant recompile on every edit to a file it never reads.
TEST_F(ShaderCompilationCacheClosureTest, SameNamedSurfaceInTwoDirs_GetSeparateClosureRows)
{
    const auto dirA = m_projectRoot / "matA";
    const auto dirB = m_projectRoot / "matB";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);

    auto write = [](const std::filesystem::path& p, const std::string& text)
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << p.string();
        out << text;
    };
    write(dirA / "helper_a.glsl", "vec3 SharedName() { return vec3(0.0, 1.0, 0.0); }\n");
    write(dirB / "helper_b.glsl", "vec3 SharedName() { return vec3(0.0, 0.0, 1.0); }\n");
    static constexpr const char* kSurfaceBody =
        "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
        "    SurfaceOutput o = DefaultSurfaceOutput();\n"
        "    o.baseColor = SharedName();\n"
        "    return o;\n"
        "}\n";
    write(dirA / "shared_surface.glsl", std::string("#include \"helper_a.glsl\"\n") + kSurfaceBody);
    write(dirB / "shared_surface.glsl", std::string("#include \"helper_b.glsl\"\n") + kSurfaceBody);

    auto makeKey = [](const std::filesystem::path& materialPath)
    {
        ShaderCacheKey k{};
        k.VariantKey.vertexFlags = VertexAttributeFlags::StandardMesh;
        k.VariantKey.lightingModel = LightingModel::kStandardPBR;
        k.Source.SurfaceShaderPath = "shared_surface.glsl";
        k.Source.MaterialAssetPath = materialPath;
        return k;
    };
    MaterialCompileSpec spec{};
    spec.surfaceShaderPath = "shared_surface.glsl";
    spec.lightingModel = "StandardPBR";

    auto vA = m_cache.GetOrCompile(makeKey(dirA / "a.material"), spec, m_ctx, "ProbeA", ShaderSourceKind::SpirV);
    ASSERT_NE(vA, nullptr);
    auto vB = m_cache.GetOrCompile(makeKey(dirB / "b.material"), spec, m_ctx, "ProbeB", ShaderSourceKind::SpirV);
    ASSERT_NE(vB, nullptr);
    ASSERT_EQ(m_cache.GetEntryCount(), 2u) << "distinct materialAssetPath => two entries";
    ASSERT_NE(vA->fragmentBytes, vB->fragmentBytes) << "the two surfaces really are different code";

    // A's helper: A drops and is named, B is untouched and still serves.
    const auto depsA = m_cache.InvalidateEntriesForShaderFile("helper_a.glsl");
    EXPECT_EQ(depsA.DroppedEntries, 1u)
        << "editing A's helper must drop A's entry and only A's";
    EXPECT_TRUE(NamesIdentity(depsA, "shared_surface.glsl", "", dirA / "a.material"))
        << "A's build must record its own helper under A's identity";
    EXPECT_FALSE(NamesIdentity(depsA, "shared_surface.glsl", "", dirB / "b.material"))
        << "B resolves a different file of the same name — it must not ride along";
    EXPECT_EQ(m_cache.GetEntryCount(), 1u) << "B's entry must survive A's helper edit";
    EXPECT_EQ(m_cache.GetOrCompile(makeKey(dirB / "b.material"), spec, m_ctx, "ProbeB", ShaderSourceKind::SpirV), vB)
        << "B's surviving entry must still serve as a hit";

    // ...and symmetrically, so the pass above is not an artifact of row order.
    const auto depsB = m_cache.InvalidateEntriesForShaderFile("helper_b.glsl");
    EXPECT_EQ(depsB.DroppedEntries, 1u);
    EXPECT_TRUE(NamesIdentity(depsB, "shared_surface.glsl", "", dirB / "b.material"));
    EXPECT_FALSE(NamesIdentity(depsB, "shared_surface.glsl", "", dirA / "a.material"));
}

TEST(ShaderCacheIdentity, RetainedKeysStayStableAndDistinctAcrossProfilesAndRehash)
{
    TestSupport::ScopedShaderCompileTarget restore{CurrentShaderCompileTarget()};
    ApplyShaderCompileProfile({SpirvTarget::Spirv_1_3, false, false});
    const ShaderCacheKey desktop = MakeTestKey("surface_a.glsl");
    const size_t originalHash = std::hash<ShaderCacheKey>{}(desktop);
    std::unordered_map<ShaderCacheKey, int> retained;
    retained.emplace(desktop, 17);
    ApplyShaderCompileProfile({SpirvTarget::Spirv_1_3, true, false});
    const ShaderCacheKey compatible = MakeTestKey("surface_a.glsl");
    EXPECT_NE(compatible, desktop);
    EXPECT_EQ(std::hash<ShaderCacheKey>{}(desktop), originalHash);
    retained.emplace(compatible, 23);
    retained.reserve(4096);
    ASSERT_EQ(retained.size(), 2u);
    EXPECT_EQ(retained.at(desktop), 17);
    EXPECT_EQ(retained.at(compatible), 23);
    ApplyShaderCompileProfile({SpirvTarget::Spirv_1_6, false, true});
    EXPECT_EQ(retained.at(desktop), 17);
    EXPECT_EQ(retained.at(compatible), 23);
    const ShaderCacheKey otherTarget = MakeTestKey("surface_a.glsl");
    EXPECT_NE(otherTarget, desktop);
    EXPECT_NE(otherTarget, compatible);
}

TEST_F(ShaderCompilationCacheTest, FailedProgramsAreCachedSeparatelyForEachStageSourceKind)
{
    const auto key = MakeTestKey("surface_a.glsl");
    const auto spec = MakeTestSpec("surface_a.glsl");
    int compiles = 0;
    m_cache.SetCompileEnteredHookForTesting([&compiles] { ++compiles; });
    EXPECT_EQ(m_cache.GetOrCompile(key, spec, m_fakeContext, "Spirv", ShaderSourceKind::SpirV), nullptr);
    EXPECT_EQ(m_cache.GetOrCompile(key, spec, m_fakeContext, "Wgsl", ShaderSourceKind::Wgsl), nullptr);
    EXPECT_EQ(compiles, 2);
    EXPECT_EQ(m_cache.GetEntryCount(), 2u);
    EXPECT_EQ(m_cache.GetOrCompile(key, spec, m_fakeContext, "SpirvAgain", ShaderSourceKind::SpirV), nullptr);
    EXPECT_EQ(compiles, 2);
}

namespace
{
std::vector<uint32_t> TargetSpirvWords(const std::vector<uint8_t>& bytes)
{
    constexpr size_t kHeaderWords = 5;
    if (bytes.size() % sizeof(uint32_t) != 0 || bytes.size() < kHeaderWords * sizeof(uint32_t))
        return {};
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    if (words[0] != spv::MagicNumber)
        return {};
    return words;
}

bool HasInterpolationCapability(const std::vector<uint8_t>& bytes)
{
    const auto words = TargetSpirvWords(bytes);
    for (size_t at = 5; at < words.size();)
    {
        const uint32_t count = words[at] >> 16u;
        if (count == 0 || at + count > words.size())
            return false;
        if ((words[at] & 0xFFFFu) == spv::OpCapability && count >= 2 &&
            words[at + 1] == spv::CapabilityInterpolationFunction)
            return true;
        at += count;
    }
    return false;
}

bool TargetMetaNames(const ShaderMeta& meta, std::string_view name)
{
    for (const auto& set : meta.Sets)
        for (const auto& binding : set.Bindings)
            if (binding.Name == name)
                return true;
    return false;
}
} // namespace

TEST_F(ShaderCompilationCacheEpochTest, InFlightReliefCompileKeepsItsCapturedTargetAfterDefaultsChange)
{
    TestSupport::ScopedShaderCompileTarget restore{CurrentShaderCompileTarget()};
    ApplyShaderCompileProfile({SpirvTarget::Spirv_1_5, false, true});
    ShaderCacheKey captured = MakeTestKey("Surfaces/standard_pbr.glsl");
    captured.VariantKey.materialKeywords |= MaterialKeyword::Parallax;
    MaterialCompileSpec spec = MakeTestSpec("Surfaces/standard_pbr.glsl");
    spec.parallax = true;
    int entered = 0;
    m_cache.SetCompileEnteredHookForTesting([&entered]
    {
        ++entered;
        ApplyShaderCompileProfile({SpirvTarget::Spirv_1_6, true, false});
    });
    auto first = m_cache.GetOrCompile(captured, spec, m_ctx, "CapturedRelief", ShaderSourceKind::SpirV);
    Disarm();
    ASSERT_EQ(entered, 1);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(first->meta, nullptr);
    for (const auto* stage : {&first->vertexBytes, &first->fragmentBytes})
    {
        const auto words = TargetSpirvWords(*stage);
        ASSERT_GE(words.size(), 5u);
        EXPECT_EQ(words[1], 0x00010500u);
    }
    EXPECT_TRUE(HasInterpolationCapability(first->fragmentBytes));
    EXPECT_TRUE(TargetMetaNames(*first->meta, "ge_BindlessTextures"));
    EXPECT_FALSE(TargetMetaNames(*first->meta, "ge_MaterialTextures0"));
    EXPECT_EQ(m_cache.GetOrCompile(captured, spec, m_ctx, "RetainedRelief", ShaderSourceKind::SpirV), first);

    ShaderCacheKey compatible = MakeTestKey("Surfaces/standard_pbr.glsl");
    compatible.VariantKey.materialKeywords |= MaterialKeyword::Parallax;
    auto second = m_cache.GetOrCompile(compatible, spec, m_ctx, "CompatibleRelief", ShaderSourceKind::SpirV);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(second->meta, nullptr);
    EXPECT_NE(second, first);
    for (const auto* stage : {&second->vertexBytes, &second->fragmentBytes})
    {
        const auto words = TargetSpirvWords(*stage);
        ASSERT_GE(words.size(), 5u);
        EXPECT_EQ(words[1], 0x00010600u);
    }
    EXPECT_FALSE(HasInterpolationCapability(second->fragmentBytes));
    EXPECT_TRUE(TargetMetaNames(*second->meta, "ge_MaterialTextures0"));
    EXPECT_FALSE(TargetMetaNames(*second->meta, "ge_BindlessTextures"));
    EXPECT_EQ(m_cache.GetEntryCount(), 2u);
}

TEST_F(ShaderCompilationCacheEpochTest, CbtHeadProgramsKeepProfileBindingsInBothInsertionOrders)
{
    TestSupport::ScopedShaderCompileTarget restore{CurrentShaderCompileTarget()};
    const auto packages = TestPaths::ExecutableDirectory() / "Assets/Shaders";
    ASSERT_TRUE(std::filesystem::is_regular_file(packages / "CBT/cbt_surface.glsl"));
    m_ctx.PackageShaderDirs.push_back(packages);
    for (const bool compatibleFirst : {false, true})
    {
        m_cache.Clear();
        std::shared_ptr<SharedShaderVariant> desktop;
        std::shared_ptr<SharedShaderVariant> compatible;
        for (const bool compatibility : {compatibleFirst, !compatibleFirst, compatibleFirst})
        {
            ApplyShaderCompileProfile({SpirvTarget::Spirv_1_5, compatibility, false});
            ShaderCacheKey key = MakeTestKey("CBT/cbt_surface.glsl");
            key.Source.VertexModifierPath = "CBT/cbt_vertex_modifier.glsl";
            key.VariantKey.vertexFlags = VertexAttributeFlags::None;
            key.VariantKey.materialKeywords = MaterialKeyword::CustomVertexShader |
                MaterialKeyword::HasVertexOutputMod | MaterialKeyword::Instanced |
                MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::IBL;
            MaterialCompileSpec spec = MakeTestSpec("CBT/cbt_surface.glsl");
            spec.vertexModifierPath = key.Source.VertexModifierPath;
            spec.customVertexShader = true;
            const auto program = m_cache.GetOrCompile(key, spec, m_ctx, "CbtProfileHead", ShaderSourceKind::SpirV);
            ASSERT_NE(program, nullptr);
            ASSERT_NE(program->meta, nullptr);
            EXPECT_EQ(TargetMetaNames(*program->meta, "gCbtChain"), !compatibility);
            EXPECT_EQ(TargetMetaNames(*program->meta, "gCbtDrawLevel"), !compatibility);
            auto& retained = compatibility ? compatible : desktop;
            if (retained)
            {
                EXPECT_EQ(program, retained);
            }
            else
            {
                retained = program;
            }
        }
        EXPECT_EQ(m_cache.GetEntryCount(), 2u);
    }
}

TEST_F(ShaderCompilationCacheEpochTest, ContentEditsInvalidateEveryRetainedTarget)
{
    TestSupport::ScopedShaderCompileTarget restore{CurrentShaderCompileTarget()};
    WriteSurface("vec3(0.0, 1.0, 0.0)");
    ApplyShaderCompileProfile({SpirvTarget::Spirv_1_5, false, false});
    const ShaderCacheKey desktop = MakeKey();
    ApplyShaderCompileProfile({SpirvTarget::Spirv_1_5, true, false});
    const ShaderCacheKey compatible = MakeKey();
    const auto desktopBefore = m_cache.GetOrCompile(desktop, MakeSpec(), m_ctx, "DesktopBefore", ShaderSourceKind::SpirV);
    const auto compatibleBefore = m_cache.GetOrCompile(compatible, MakeSpec(), m_ctx, "CompatibleBefore", ShaderSourceKind::SpirV);
    ASSERT_NE(desktopBefore, nullptr);
    ASSERT_NE(compatibleBefore, nullptr);
    ASSERT_EQ(m_cache.GetEntryCount(), 2u);

    WriteSurface("vec3(1.0, 0.0, 0.0)");
    m_cache.InvalidateGlobalEntry(desktop);
    EXPECT_EQ(m_cache.GetEntryCount(), 0u);
    const auto desktopAfter = m_cache.GetOrCompile(desktop, MakeSpec(), m_ctx, "DesktopAfter", ShaderSourceKind::SpirV);
    const auto compatibleAfter = m_cache.GetOrCompile(compatible, MakeSpec(), m_ctx, "CompatibleAfter", ShaderSourceKind::SpirV);
    ASSERT_NE(desktopAfter, nullptr);
    ASSERT_NE(compatibleAfter, nullptr);
    EXPECT_NE(desktopAfter->fragmentBytes, desktopBefore->fragmentBytes);
    EXPECT_NE(compatibleAfter->fragmentBytes, compatibleBefore->fragmentBytes);
}

TEST_F(ShaderCompilationCacheEpochTest, AnotherTargetCompilesWhileTheFirstTargetIsHeld)
{
    TestSupport::ScopedShaderCompileTarget restore{CurrentShaderCompileTarget()};
    ApplyShaderCompileProfile({SpirvTarget::Spirv_1_5, false, false});
    WriteSurface("vec3(0.0, 1.0, 0.0)");
    const ShaderCacheKey desktop = MakeKey();
    std::promise<void> firstEntered;
    std::promise<void> releaseFirst;
    const auto release = releaseFirst.get_future().share();
    std::atomic<uint32_t> entered{0};
    m_cache.SetCompileEnteredHookForTesting([&entered, &firstEntered, release]
    {
        if (entered.fetch_add(1) == 0u)
        {
            firstEntered.set_value();
            release.wait();
        }
    });
    auto first = std::async(std::launch::async, [this, desktop]
    {
        return m_cache.GetOrCompile(desktop, MakeSpec(), m_ctx, "HeldDesktop", ShaderSourceKind::SpirV);
    });
    const bool started = firstEntered.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    ApplyShaderCompileProfile({SpirvTarget::Spirv_1_6, true, false});
    const ShaderCacheKey compatible = MakeKey();
    std::future<std::shared_ptr<SharedShaderVariant>> second;
    try
    {
        second = std::async(std::launch::async, [this, compatible]
        {
            return m_cache.GetOrCompile(compatible, MakeSpec(), m_ctx, "ConcurrentCompatible", ShaderSourceKind::SpirV);
        });
    }
    catch (...)
    {
        releaseFirst.set_value();
        first.wait();
        throw;
    }
    const bool independentlyFinished = second.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    releaseFirst.set_value();
    const auto desktopProgram = first.get();
    const auto compatibleProgram = second.get();
    Disarm();
    EXPECT_TRUE(started);
    EXPECT_TRUE(independentlyFinished);
    EXPECT_EQ(entered.load(), 2u);
    ASSERT_NE(desktopProgram, nullptr);
    ASSERT_NE(compatibleProgram, nullptr);
    EXPECT_NE(desktopProgram, compatibleProgram);
    const auto desktopWords = TargetSpirvWords(desktopProgram->vertexBytes);
    const auto compatibleWords = TargetSpirvWords(compatibleProgram->vertexBytes);
    ASSERT_GE(desktopWords.size(), 5u);
    ASSERT_GE(compatibleWords.size(), 5u);
    EXPECT_EQ(desktopWords[1], 0x00010500u);
    EXPECT_EQ(compatibleWords[1], 0x00010600u);
    EXPECT_EQ(m_cache.GetEntryCount(), 2u);
}
