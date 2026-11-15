#pragma once

#include <functional>
#include <memory>

namespace GameEngine
{
class UIElement;

namespace EditorUI
{

/// A refresh that runs once at the next UI drain after one or more Request calls.
///
/// The owner (usually the element passed as `host`) keeps this object as a member. A posted refresh
/// that drains after this object is destroyed does nothing, so it never touches a destroyed owner.
/// UI thread only.
class PostedRefresh
{
public:
    /// `host` posts the action (UIElement::PostAction) and must outlive this object. `refresh` runs on
    /// the drain.
    PostedRefresh(UIElement& host, std::function<void()> refresh);
    ~PostedRefresh();

    PostedRefresh(const PostedRefresh&) = delete;
    PostedRefresh& operator=(const PostedRefresh&) = delete;

    /// Posts the refresh unless one is already pending. A post that the host drops releases the
    /// pending state, so the next Request posts again.
    void Request();

    /// True from a posted Request to its drain.
    bool IsPending() const;

private:
    struct State;
    // The posted action holds the state weakly: once the owner destroys this object, the action does nothing.
    static void RunIfAlive(const std::weak_ptr<State>& weakState);

    std::shared_ptr<State> m_State;
};

} // namespace EditorUI
} // namespace GameEngine
