#include "Benchmark/PlayerBenchmark.h"

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "Components/TransformDirtyFeed.h"
#include "Core/DebugMetrics.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/Camera.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace GameEngine
{

namespace
{

// Pass timings resolve when the device frame slot returns, frames in flight later. The profiled
// window starts this many frames after profiling is armed, so its first frame is resolved.
constexpr uint32_t kProfileLeadFrames = 4;

} // namespace

PlayerBenchmark::PlayerBenchmark(PlayerBenchmarkPlan plan, std::filesystem::path outputPath)
    : m_Plan(std::move(plan))
    , m_OutputPath(std::move(outputPath))
    , m_Records(m_Plan.Poses.size())
{
    for (PoseRecord& record : m_Records)
        record.MonitorPeaks.resize(m_Plan.ActiveMonitors.size());
}

bool PlayerBenchmark::Advance(ECS::World& world, Rendering::RenderGraph::RGFrame* frame,
                              const Rendering::IDevice* device, bool previousFrameSubmitted)
{
    if (m_Phase == Phase::Done)
        return true;
    if (UsesTimingPath() && m_LastAdvance && !previousFrameSubmitted)
        return FailRoute("the previous frame was not submitted", frame);

    const auto now = std::chrono::steady_clock::now();
    const std::optional<double> frameMs =
        m_LastAdvance ? std::optional<double>(std::chrono::duration<double, std::milli>(now - *m_LastAdvance).count())
                      : std::nullopt;
    m_LastAdvance = now;

    // The frame that ended rendered the pose of the current phase; record it, then step.
    if (frameMs && m_Phase == Phase::Timing)
        RecordTimingFrame(*frameMs, device);
    else if (m_Phase == Phase::Profiled && frame)
        RecordProfiledFrame(*frame);
    ++m_PhaseFrames;

    switch (m_Phase)
    {
        case Phase::Warmup:
            if (m_PhaseFrames > m_Plan.WarmupFrames)
                EnterPhase(Phase::Timing, frame);
            break;
        case Phase::Timing:
            if (m_Records[m_PoseIndex].FrameMs.size() >= m_Plan.TimingFrames)
                EnterPhase(Phase::ProfileLead, frame);
            break;
        case Phase::ProfileLead:
            if (m_PhaseFrames >= kProfileLeadFrames)
                EnterPhase(Phase::Profiled, frame);
            break;
        case Phase::Profiled:
            if (m_PhaseFrames >= m_Plan.ProfiledFrames)
            {
                if (frame)
                    frame->SetProfilingEnabled(false);
                if (++m_PoseIndex < m_Plan.Poses.size())
                    EnterPhase(Phase::Warmup, frame);
                else
                    EnterPhase(Phase::Done, frame);
            }
            break;
        case Phase::Done:
            break;
    }

    if (m_Phase == Phase::Done)
    {
        WriteResult();
        return true;
    }
    if (!ApplyPose(world) && UsesTimingPath())
        return FailRoute("the scene needs an active root camera with a Transform", frame);
    return false;
}

bool PlayerBenchmark::FailRoute(const char* reason, Rendering::RenderGraph::RGFrame* frame)
{
    Logger::Log::Error("PlayerBenchmark: timingPath stopped because {}.", reason);
    if (frame)
        frame->SetProfilingEnabled(false);
    EnterPhase(Phase::Done, frame);
    return true;
}

void PlayerBenchmark::EnterPhase(Phase phase, Rendering::RenderGraph::RGFrame* frame)
{
    m_Phase = phase;
    m_PhaseFrames = 0;
    if (phase == Phase::ProfileLead && frame)
        frame->SetProfilingEnabled(true);
    if (phase == Phase::Warmup || phase == Phase::Done)
        return;
    if (phase == Phase::Timing && !m_Plan.TimingPath.empty())
        Logger::Log::Info("PlayerBenchmark: replaying {} ordered camera poses", m_Plan.TimingPath.size());
    Logger::Log::Info("PlayerBenchmark: pose '{}' {}", m_Plan.Poses[m_PoseIndex].Name,
                      phase == Phase::Timing ? "timing" : (phase == Phase::ProfileLead ? "profiling" : "profiled window"));
}

bool PlayerBenchmark::ApplyPose(ECS::World& world) const
{
    const std::optional<Engine::Renderer::Camera> camera = Engine::Renderer::FindActiveCamera(world);
    if (!camera)
        return false;
    if (!m_Plan.TimingPath.empty())
    {
        if (const auto* parent = world.GetComponent<Components::Parent>(camera->entity);
            parent && parent->Entity.IsValid())
            return false;
    }
    auto* transform = world.GetComponentForWrite<Components::Transform>(camera->entity);
    if (!transform)
        return false;
    const PlayerBenchmarkPose* pose = &m_Plan.Poses[m_PoseIndex];
    if (!m_Plan.TimingPath.empty())
    {
        if (m_Phase == Phase::Timing)
            pose = &m_Plan.TimingPath[m_PhaseFrames];
        else if (m_Phase == Phase::ProfileLead || m_Phase == Phase::Profiled)
            pose = &m_Plan.TimingPath.back();
    }
    *transform = Components::Transform::FromTRS(pose->Position, pose->Rotation(), Mathematics::Vector3(1.0f, 1.0f, 1.0f));
    // A root camera: its world transform is its transform. Write both, as the camera controller does.
    if (auto* worldTransform = world.GetComponentForWrite<Components::WorldTransform>(camera->entity))
    {
        worldTransform->Position = transform->Position;
        worldTransform->Basis = transform->Basis;
        Components::BumpWorldTransform(world, camera->entity, *worldTransform);
    }
    return true;
}

void PlayerBenchmark::RecordTimingFrame(double frameMs, const Rendering::IDevice* device)
{
    double waitMs = 0.0;
    Rendering::IDevice::FrameSyncTimings sync{};
    if (device && device->GetLastFrameSyncTimings(sync))
        waitMs = sync.beginFrameWaitMs + sync.acquireMs + sync.presentMs;
    PoseRecord& record = m_Records[m_PoseIndex];
    record.FrameMs.push_back(frameMs);
    record.WaitMs.push_back(waitMs);
    record.CpuMs.push_back(std::max(0.0, frameMs - waitMs));
}

void PlayerBenchmark::RecordProfiledFrame(const Rendering::RenderGraph::RGFrame& frame)
{
    PoseRecord& record = m_Records[m_PoseIndex];
    for (size_t index = 0; index < m_Plan.ActiveMonitors.size(); ++index)
    {
        float value = 0.0f;
        if (Debug::DebugMetrics::Get().GetLatest(m_Plan.ActiveMonitors[index], value))
            record.MonitorPeaks[index] = std::max(record.MonitorPeaks[index], value);
    }

    const Rendering::RenderGraph::RGFrame::RGProfilingSnapshot snapshot = frame.LastFrameProfiling();
    if (snapshot.ResolveStats.ResolvedPasses == 0)
        return;
    ProfiledFrame profiled;
    profiled.DistinctGpuMs = snapshot.ResolveStats.DistinctSpanGpuMs;
    profiled.Passes.reserve(snapshot.Passes.size());
    for (const auto& pass : snapshot.Passes)
        profiled.Passes.push_back({pass.Name, pass.GpuSpanMs, pass.SpanCounted});
    record.Profiled.push_back(std::move(profiled));
}

void PlayerBenchmark::WriteResult()
{
    nlohmann::json poses = nlohmann::json::array();
    for (size_t i = 0; i < m_Plan.Poses.size(); ++i)
    {
        const PlayerBenchmarkPose& pose = m_Plan.Poses[i];
        const PoseRecord& record = m_Records[i];
        nlohmann::json monitorPeaks = nlohmann::json::object();
        for (size_t index = 0; index < m_Plan.ActiveMonitors.size(); ++index)
            monitorPeaks[m_Plan.ActiveMonitors[index]] = record.MonitorPeaks[index];
        nlohmann::json profiled = nlohmann::json::array();
        for (const ProfiledFrame& frame : record.Profiled)
        {
            nlohmann::json passes = nlohmann::json::array();
            for (const PassSample& pass : frame.Passes)
                passes.push_back({pass.Name, pass.GpuMs, pass.Counted});
            profiled.push_back({{"distinctGpuMs", frame.DistinctGpuMs}, {"passes", std::move(passes)}});
        }
        poses.push_back({{"name", pose.Name},
                         {"position", {pose.Position.X, pose.Position.Y, pose.Position.Z}},
                         {"yawDeg", pose.YawDeg},
                         {"pitchDeg", pose.PitchDeg},
                         {"frameMs", record.FrameMs},
                         {"cpuMs", record.CpuMs},
                         {"waitMs", record.WaitMs},
                         {"monitorPeaks", std::move(monitorPeaks)},
                         {"profiledFrames", std::move(profiled)}});
    }
    nlohmann::json result = {{"warmupFrames", m_Plan.WarmupFrames},
                             {"timingFrames", m_Plan.TimingFrames},
                             {"profiledFrames", m_Plan.ProfiledFrames},
                             {"poses", std::move(poses)}};

    if (!m_Plan.TimingPath.empty())
    {
        auto& path = result["timingPath"];
        path = nlohmann::json::array();
        for (const auto& pose : m_Plan.TimingPath)
            path.push_back({{"position", {pose.Position.X, pose.Position.Y, pose.Position.Z}},
                            {"yawDeg", pose.YawDeg},
                            {"pitchDeg", pose.PitchDeg}});
        result["profiledPose"] = path.back();
    }

    std::ofstream out(m_OutputPath, std::ios::binary | std::ios::trunc);
    out << result.dump();
    out.close();
    m_Succeeded = static_cast<bool>(out);
    if (m_Succeeded)
        Logger::Log::Info("PlayerBenchmark: wrote '{}'", m_OutputPath.string());
    else
        Logger::Log::Error("PlayerBenchmark: cannot write '{}'. Check that its folder exists and is writable.",
                           m_OutputPath.string());
}

} // namespace GameEngine
