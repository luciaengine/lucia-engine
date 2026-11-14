#include "Platform/Toolbar.h"

namespace GameEngine
{
std::unique_ptr<INativeToolbar> CreateNativeToolbar()
{
    return nullptr;
}
} // namespace GameEngine
