// This file must compile on every compiler. If it fails, the header or its include path is broken,
// and a failure of FunctionRefBoundToTemporary proves nothing.
#include "Types/FunctionRef.h"

int CallStoredReference()
{
    auto addOne = [](int value) { return value + 1; };
    GameEngine::FunctionRef<int(int)> stored = addOne;
    return stored(1);
}
