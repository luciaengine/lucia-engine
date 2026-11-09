#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

// Marks a parameter that the returned or constructed object refers to. Clang then warns when that object
// outlives a temporary argument. Other compilers get an empty macro: MSVC reports C5030 for an unknown attribute, and /WX makes it an error.
#if __has_cpp_attribute(clang::lifetimebound)
#define GE_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define GE_LIFETIMEBOUND
#endif

namespace GameEngine
{

template <typename Signature>
class FunctionRef;

/// Refers to a callable and does not own it. A function that takes a FunctionRef calls it only before that function returns.
/// Use it only as a parameter type: never a member, never a return value. Use std::function to keep a callback.
/// It is the size of two pointers and is trivially copyable. Pass it by value.
template <typename Result, typename... Arguments>
class FunctionRef<Result(Arguments...)>
{
  public:
    /// The callable must live until the call that takes this FunctionRef returns.
    template <typename Callable>
        requires(!std::is_same_v<std::remove_cvref_t<Callable>, FunctionRef>) &&
                std::is_invocable_r_v<Result, Callable&, Arguments...>
    FunctionRef(Callable&& callable GE_LIFETIMEBOUND) noexcept
        : m_Callable(reinterpret_cast<std::uintptr_t>(std::addressof(callable)))
        , m_InvokeCallable(&InvokeCallable<std::remove_reference_t<Callable>>)
    {
    }

    FunctionRef(const FunctionRef&) noexcept = default;
    /// Deleted: an assignment can make a FunctionRef refer to a temporary.
    FunctionRef& operator=(const FunctionRef&) = delete;

    Result operator()(Arguments... arguments) const
    {
        return m_InvokeCallable(m_Callable, std::forward<Arguments>(arguments)...);
    }

  private:
    // An integer holds the address of a const object and of a function, so no cast removes const.
    template <typename Callable>
    static Result InvokeCallable(std::uintptr_t callable, Arguments... arguments)
    {
        if constexpr (std::is_void_v<Result>)
            (*reinterpret_cast<Callable*>(callable))(std::forward<Arguments>(arguments)...);
        else
            return (*reinterpret_cast<Callable*>(callable))(std::forward<Arguments>(arguments)...);
    }

    std::uintptr_t m_Callable;
    Result (*m_InvokeCallable)(std::uintptr_t, Arguments...);
};

} // namespace GameEngine
