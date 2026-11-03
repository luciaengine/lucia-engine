// Parallel passes a Write<T> column as Writable<T>: a T& parameter would write
// without recording the write, so no Changed<T> consumer would see it.
#include "CompileFailCommon.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    world.Query<Write<Velocity>>().Parallel([](EntityHandle, Velocity& v) { v.x = 1.0f; });
}
