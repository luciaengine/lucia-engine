// Changed<T> requires T to be declared Read<T>: the query's own writes stamp a
// column it writes, so a gate on that column would re-trigger itself.
#include "CompileFailCommon.h"
#include "ECS/ChangeFilter.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    GameEngine::ECS::ChangeGate gate;
    auto q = world.Query<Read<Position>, Write<Velocity>>();
    q.Changed<Velocity>(gate);
    q.Each([](EntityHandle, const Position&, Writable<Velocity> v) { v.Mutable().x = 1.0f; });
}
