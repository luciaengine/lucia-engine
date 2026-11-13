// A tree restart is not visible (#2662): a tree re-seeded from its roots over resident heights draws
// its first frame at least as fine at the camera as the settled tree it replaces. Real-device arm
// over a 576 m relief terrain (heightScale 60, cap 21), the camera at eye height looking ahead:
//
//   RestartDrawsTheSettledTreeAtTheCameraInItsFirstFrame — the restart (InitializeRoots, the uniform
//     seed, RefineToView) and the frame's own update draw no fewer and no coarser leaves near the
//     camera than the settled tree drew in the frame before.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTTreeAudit.h"
#include "CBTReliefHeightSource.h"
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 900.0f;
constexpr float kFovY = 1.05f;
constexpr float kTerrainM = 576.0f;
constexpr uint32_t kLatticeDim = 1025u;
constexpr uint32_t kCap = 21u;
constexpr float kHeightScaleM = 60.0f;
constexpr float kEyeM = 1.7f;
constexpr float kTargetPx = 8.0f;
constexpr float kFlatPx = 1.0f;
constexpr uint32_t kSettleFrames = 120u;
// CBTTreeSeed::kRestartSeedDepth: the uniform depth a restart seeds before its first draw.
constexpr uint32_t kRestartSeedDepth = 14u;
// The disc around the camera whose leaves a viewer sees up close.
constexpr float kNearRadiusM = 30.0f;

uint32_t HeapDepth(uint64_t heapId)
{
    uint32_t depth = 0;
    while (heapId > 1u)
    {
        heapId >>= 1u;
        ++depth;
    }
    return depth;
}

struct NearField
{
    uint32_t Leaves = 0;
    uint32_t Deepest = 0;
    uint32_t Live = 0;
    bool Watertight = false;
};

// The live leaves whose centroid lies within kNearRadiusM of the camera's terrain-local (x, z).
NearField MeasureNearField(CBTInstance& inst, float cameraX, float cameraZ)
{
    const CBTTreeSnapshot snapshot = ReadTreeSnapshot(inst, true);
    const CBTTreeAuditResult audit = AuditTree(snapshot);
    NearField field;
    field.Live = audit.LiveCount;
    field.Watertight = audit.Watertight();
    for (uint32_t slot = 0; slot < snapshot.PoolSize; ++slot)
    {
        if (snapshot.HeapIds[slot] == 0u)
            continue;
        const CBTVertexData& v = snapshot.Vertices[slot];
        const float x = (v.Corner0[3] + v.Corner1[3] + v.Corner2[3]) * (kTerrainM / 3.0f) - cameraX;
        const float z = (v.Meta[0] + v.Meta[1] + v.Meta[2]) * (kTerrainM / 3.0f) - cameraZ;
        if (x * x + z * z > kNearRadiusM * kNearRadiusM)
            continue;
        ++field.Leaves;
        field.Deepest = std::max(field.Deepest, HeapDepth(snapshot.HeapIds[slot]));
    }
    return field;
}

class CBTRestartRefine : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "no headless Vulkan device";
        if (!m_Device->GetCapabilities().supportsShaderInt64)
            GTEST_SKIP() << "device lacks shaderInt64";
        if (!m_KernelSet.Initialize(*m_Device, ShaderOutputDir()))
            GTEST_SKIP() << "cbt_kernels.comp.spv missing (glslc unavailable at build)";
    }
    void TearDown() override
    {
        m_KernelSet.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    // The camera at terrain-local (x, eyeY, z), looking 10 degrees down along +Z.
    static CBTFrameParams CameraParams(float x, float eyeY, float z)
    {
        using namespace GameEngine::Mathematics;
        CBTFrameParams p{};
        p.Screen[0] = kScreenW;
        p.Screen[1] = kScreenH;
        p.Screen[2] = kTargetPx;
        p.Screen[3] = kTargetPx * 0.5f;
        p.TerrainSize[0] = kTerrainM;
        p.TerrainSize[1] = kTerrainM;
        p.TerrainSize[2] = kHeightScaleM;
        p.TerrainOrigin[2] = static_cast<float>(kCap);
        const Vector3 eye(x, eyeY, z);
        const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(kFovY, kScreenW / kScreenH, 0.5f, 20000.0f);
        const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + Vector3(0.0f, -0.176f, 1.0f), Vector3(0, 1, 0));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x;
        p.CameraPos[1] = eye.y;
        p.CameraPos[2] = eye.z;
        p.CameraPos[3] = 1.0f;
        const CBTNearBiasRadii nb = ComputeNearBiasRadii(kEyeM);
        p.NearBias[0] = 1.0f;
        p.NearBias[1] = nb.NearRadius;
        p.NearBias[2] = nb.FarRadius;
        p.NearBias[3] = kNearBiasMaxCoarsen;
        p.DemandTuning[0] = kNearFieldFacetTargetM;
        p.DemandTuning[1] = kOffFrustumKeepOcc;
        p.WaterPlane[3] = kFlatPx;
        return p;
    }

    static CBTClassifyDesc ScreenClassify()
    {
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = kCap;
        c.NearFieldGate = 1u;
        c.GateVertexEval = 1u;
        return c;
    }

    // One frame's update, as CBTRenderNode records it; `forceVertexEval` re-evaluates every corner.
    void RunFrame(CBTInstance& inst, const CBTFrameParams& p, uint32_t frame, bool forceVertexEval)
    {
        CBTClassifyDesc c = ScreenClassify();
        c.GateVertexEval = forceVertexEval ? 0u : 1u;
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        inst.RecordUpdate(*cl, c, p, frame);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};
} // namespace

TEST_F(CBTRestartRefine, RestartDrawsTheSettledTreeAtTheCameraInItsFirstFrame)
{
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet) && inst.InitializeRoots(kDomainPlanar));
    ReliefHeightSource relief(*m_Device, inst, kHeightScaleM, kLatticeDim);
    ASSERT_TRUE(relief.IsArmed());

    const float cameraX = kTerrainM * 0.5f;
    const float cameraZ = kTerrainM * 0.5f;
    const CBTFrameParams p = CameraParams(cameraX, ReliefHeightAt(0.5f, 0.5f) * kHeightScaleM + kEyeM, cameraZ);
    for (uint32_t frame = 0; frame < kSettleFrames; ++frame)
        RunFrame(inst, p, frame, frame == 0u);
    const NearField settled = MeasureNearField(inst, cameraX, cameraZ);

    // The restart frame, as CBTRenderFeature runs it: re-seed, refine, then the frame's own update.
    const uint32_t restartFrame = kSettleFrames;
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    inst.RefineUniform(kRestartSeedDepth, restartFrame);
    inst.RefineToView(ScreenClassify(), p, kRestartSeedDepth, restartFrame);
    RunFrame(inst, p, restartFrame, true);
    const NearField restarted = MeasureNearField(inst, cameraX, cameraZ);

    std::printf("settled: live %u, near %u leaves (deepest %u); restart frame: live %u, near %u leaves "
                "(deepest %u)\n",
                settled.Live, settled.Leaves, settled.Deepest, restarted.Live, restarted.Leaves,
                restarted.Deepest);
    ASSERT_EQ(settled.Deepest, kCap) << "the settled tree does not reach the cap at the camera; the arm is void";
    EXPECT_TRUE(restarted.Watertight);
    EXPECT_GE(restarted.Deepest, settled.Deepest) << "the restart frame draws the near field coarser";
    EXPECT_GE(restarted.Leaves, settled.Leaves) << "the restart frame draws fewer leaves near the camera";
    inst.Shutdown();
}
