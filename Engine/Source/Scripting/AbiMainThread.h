#pragma once

// AbiMainThread.h - The scripting ABI's rule for exports that may run only on the engine's main thread.

#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/ScriptManager.h"

namespace GameEngine::ScriptingAbi
{

/// True on the initialized engine main thread, outside a worker or inline job body.
inline bool IsMainThreadOutsideJobFor(const EngineCore& engine)
{
    return engine.IsMainThread() && !JobSystem::WorkStealingThreadPool::IsExecutingJob();
}

/// True when no script main thread is marked, or the caller is that thread. Standalone hosts have no
/// frame loop. Resolve ScriptManager per call because engine initialization and shutdown replace it.
inline bool OnEngineMainThread()
{
    const ScriptManager& scripts = EngineCore::GetInstance().GetScriptManager();
    return !scripts.HasMarkedMainThread() || scripts.IsMainThread();
}

} // namespace GameEngine::ScriptingAbi
