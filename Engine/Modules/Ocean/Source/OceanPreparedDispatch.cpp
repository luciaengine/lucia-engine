#include "Ocean/OceanPreparedDispatch.h"

#include "Rendering/Core/CommandList.h"

namespace GameEngine::Ocean
{
bool RecordOceanPreparedDispatch(Rendering::CommandList* commands, const OceanPreparedDispatch& dispatch)
{
    if (!commands || !dispatch.IsValid())
        return false;
    commands->SetPipeline(dispatch.Pipeline);
    commands->BindDescriptorSet(0, dispatch.Set, dispatch.Pipeline);
    commands->Dispatch(dispatch.GroupsX, dispatch.GroupsY, dispatch.GroupsZ);
    return true;
}
} // namespace GameEngine::Ocean
