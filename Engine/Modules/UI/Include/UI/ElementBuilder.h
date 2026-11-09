#pragma once

#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"

#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace GameEngine::UI
{
namespace Detail
{
template<class Name>
concept ElementName = std::convertible_to<Name, std::string_view>;

template<class Child>
concept OwnedElementChild = std::convertible_to<Child, std::unique_ptr<UIElement>> ||
    requires(Child&& child)
    {
        { std::forward<Child>(child).Build() } -> std::convertible_to<std::unique_ptr<UIElement>>;
    };

template<class Setter, class T, class... Arguments>
concept ElementSetter = std::is_member_function_pointer_v<Setter> &&
    std::is_invocable_v<Setter, T&, Arguments...> &&
    std::is_void_v<std::invoke_result_t<Setter, T&, Arguments...>>;
}

/** Builds a retained control subtree on the UI thread. Owns its controls until Build
 * transfers ownership. A failed child destroys the incomplete subtree. Styling belongs
 * in CSS; Set invokes control setters and does not author XML attributes.
 */
template<class T> requires std::derived_from<T, UIElement>
class ElementBuilder
{
public:
    /** Creates T through its registered factory and applies registered defaults.
     * An empty tag requires one canonical registration; an explicit tag may be an alias.
     */
    explicit ElementBuilder(std::string_view tag = {});
    ElementBuilder(ElementBuilder&&) noexcept = default;
    ElementBuilder& operator=(ElementBuilder&&) noexcept = default;
    ElementBuilder(const ElementBuilder&) = delete;
    ElementBuilder& operator=(const ElementBuilder&) = delete;

    /** Assigns the element's identity using the existing ID rules. */
    ElementBuilder& Id(std::string_view id) &;
    ElementBuilder&& Id(std::string_view id) &&;

    /** Adds one or more CSS classes in argument order using existing deduplication rules. */
    template<Detail::ElementName... ClassNames>
    ElementBuilder& Class(std::string_view className, ClassNames&&... classNames) &;
    template<Detail::ElementName... ClassNames>
    ElementBuilder&& Class(std::string_view className, ClassNames&&... classNames) &&;

    /** Immediately invokes a void member setter, including inherited setters. */
    template<class Setter, class... Arguments>
        requires Detail::ElementSetter<Setter, T, Arguments...>
    ElementBuilder& Set(Setter setter, Arguments&&... arguments) &;
    template<class Setter, class... Arguments>
        requires Detail::ElementSetter<Setter, T, Arguments...>
    ElementBuilder&& Set(Setter setter, Arguments&&... arguments) &&;

    /** Appends owned children in argument order through virtual AddChild. Accepts
     * rvalue builders and unique_ptr subtrees, including trees parsed from XML.
     * Any null child invalidates the parent; all arguments are still consumed.
     */
    template<class... ChildrenTypes> requires (Detail::OwnedElementChild<ChildrenTypes> && ...)
    ElementBuilder& Children(ChildrenTypes&&... children) &;
    template<class... ChildrenTypes> requires (Detail::OwnedElementChild<ChildrenTypes> && ...)
    ElementBuilder&& Children(ChildrenTypes&&... children) &&;

    /** Registers an element-lifetime handler without wrapping its callable. For an
     * independently revocable subscription, use RegisterEventHandler after Build.
     */
    ElementBuilder& On(EventId id, UIElement::EventHandler handler) &;
    ElementBuilder&& On(EventId id, UIElement::EventHandler handler) &&;

    /** Assigns a weak reference immediately; retains no reference to the destination.
     * The reference expires if construction fails or the owning subtree is destroyed.
     */
    ElementBuilder& Capture(UIElement::WeakRef<T>& reference) &;
    ElementBuilder&& Capture(UIElement::WeakRef<T>& reference) &&;

    /** Consumes the declaration. Returns null for a failed or already consumed builder.
     * Attach only a successful result through AddChild or UIManager::SetRoot.
     */
    [[nodiscard]] std::unique_ptr<T> Build() &&;

private:
    template<class Child>
    void Append(Child&& child);

    std::unique_ptr<T> m_Element;
};

/** Starts a typed retained declaration on the UI thread. Custom controls must be
 * registered, including custom factories for controls requiring constructor arguments.
 */
template<class T> requires std::derived_from<T, UIElement>
[[nodiscard]] ElementBuilder<T> Element(std::string_view tag = {});
}

#include "UI/ElementBuilder.inl"
