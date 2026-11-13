#include "Panels/Animation/ElementInstanceLedger.h"

#include "UI/UIElement.h"

namespace GameEngine::Editor
{

bool ElementInstanceLedger::ClaimNew(const UIElement* element)
{
    if (!element)
        return false;
    const std::uint64_t id = element->GetInstanceId();
    if (m_Recorded.Contains(id))
        return false;
    m_ThisPass.push_back(id);
    return true;
}

void ElementInstanceLedger::EndPass()
{
    for (const std::uint64_t id : m_ThisPass)
        m_Recorded.Insert(id);
    m_ThisPass.clear();
}

} // namespace GameEngine::Editor
