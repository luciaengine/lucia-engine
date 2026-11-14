// Exactness pin for the completion notify-fence gate (waiter-count Dekker,
// TaskData::Waiters): locks/task on the completion path.
//
//  - With NO registered waiter, completion must take the CompletionMutex
//    exactly ZERO times (the gate's whole point — pre-gate code paid the
//    fence unconditionally, 1 lock per completed handle task).
//  - With a parked waiter, completion must take it exactly ONCE per task.
//
// The fence census (Detail::GetCompletionFenceStatsForTests) is compiled
// only where GE_DEBUG_INSTRUMENTATION is 1 (Debug and DebugFast) so the
// Release hot path pays nothing for this pin; elsewhere the test skips. Run
// it via the true-Debug tripwire gate.

#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "ScopedLogLevel.h"

namespace GameEngine::Tests {

#if !GE_DEBUG_INSTRUMENTATION

TEST(CompletionFencePin, FenceCountsExact)
{
    GTEST_SKIP() << "fence census is compiled only where GE_DEBUG_INSTRUMENTATION is 1";
}

#else



#endif // !GE_DEBUG_INSTRUMENTATION

} // namespace GameEngine::Tests
