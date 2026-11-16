#include "UI/PostedRefresh.h"

#include "UI/UIElement.h"

#include <utility>

namespace GameEngine::EditorUI
{

struct PostedRefresh::State
{
    UIElement* Host = nullptr;
    std::function<void()> Refresh;
    bool Pending = false;
};

void PostedRefresh::RunIfAlive(const std::weak_ptr<State>& weakState)
{
    const std::shared_ptr<State> state = weakState.lock();
    if (!state)
        return;
    state->Pending = false;
    if (state->Refresh)
        state->Refresh();
}

PostedRefresh::PostedRefresh(UIElement& host, std::function<void()> refresh)
    : m_State(std::make_shared<State>())
{
    m_State->Host = &host;
    m_State->Refresh = std::move(refresh);
}

PostedRefresh::~PostedRefresh() = default;

void PostedRefresh::Request()
{
    if (m_State->Pending)
        return;
    m_State->Pending = true;
    const std::weak_ptr<State> weakState = m_State;
    if (!m_State->Host->PostAction([weakState]() { RunIfAlive(weakState); }))
        m_State->Pending = false;
}

bool PostedRefresh::IsPending() const
{
    return m_State->Pending;
}

} // namespace GameEngine::EditorUI
