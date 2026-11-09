// This file must fail to compile under clang: the FunctionRef outlives the temporary lambda that it refers to.
#include "Types/FunctionRef.h"

int CallStoredReference()
{
    GameEngine::FunctionRef<int(int)> stored = [](int value) { return value + 1; };
    return stored(1);
}
