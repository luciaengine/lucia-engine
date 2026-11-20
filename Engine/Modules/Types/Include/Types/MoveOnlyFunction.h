#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace GameEngine
{

template <typename Signature>
class MoveOnlyFunction;

/// Owns a callable that it moves but does not copy. Use it for a callback that one owner keeps and calls
/// later, for example on another thread. Use FunctionRef for a callback that is called before the call
/// that takes it returns.
/// A callable of at most kInlineBytes that moves without throwing is stored in the object, so storing it
/// does not allocate. A larger callable is stored on the heap. std::function stores in place only what its
/// library allows: libstdc++ stores only small trivially copyable callables, so a lambda that captures a
/// reference-counted handle allocates there.
/// Like std::function, a const MoveOnlyFunction calls its callable as a non-const object.
template <typename Result, typename... Arguments>
class MoveOnlyFunction<Result(Arguments...)>
{
  public:
    /// The largest callable that is stored in the object.
    static constexpr size_t kInlineBytes = 48;

    MoveOnlyFunction() noexcept = default;
    MoveOnlyFunction(std::nullptr_t) noexcept {}

    /// Stores `callable`. A null function pointer or an empty std::function makes an empty MoveOnlyFunction.
    template <typename Callable>
        requires(!std::is_same_v<std::remove_cvref_t<Callable>, MoveOnlyFunction>) &&
                std::is_invocable_r_v<Result, std::decay_t<Callable>&, Arguments...>
    MoveOnlyFunction(Callable&& callable)
    {
        Store(std::forward<Callable>(callable));
    }

    MoveOnlyFunction(MoveOnlyFunction&& other) noexcept { TakeFrom(other); }

    MoveOnlyFunction& operator=(MoveOnlyFunction&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            TakeFrom(other);
        }
        return *this;
    }

    MoveOnlyFunction& operator=(std::nullptr_t) noexcept
    {
        Reset();
        return *this;
    }

    template <typename Callable>
        requires(!std::is_same_v<std::remove_cvref_t<Callable>, MoveOnlyFunction>) &&
                std::is_invocable_r_v<Result, std::decay_t<Callable>&, Arguments...>
    MoveOnlyFunction& operator=(Callable&& callable)
    {
        Reset();
        Store(std::forward<Callable>(callable));
        return *this;
    }

    MoveOnlyFunction(const MoveOnlyFunction&) = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

    ~MoveOnlyFunction() { Reset(); }

    explicit operator bool() const noexcept { return m_Operations != nullptr; }

    /// Calls the callable. Calling an empty MoveOnlyFunction is undefined: check it first.
    Result operator()(Arguments... arguments) const
    {
        return m_Operations->Invoke(m_Storage, std::forward<Arguments>(arguments)...);
    }

  private:
    struct Operations
    {
        Result (*Invoke)(void* storage, Arguments&&... arguments);
        // Moves the callable from `from` into the empty `to`, and destroys what is left in `from`.
        void (*Relocate)(void* from, void* to) noexcept;
        void (*Destroy)(void* storage) noexcept;
    };

    template <typename Callable>
    static constexpr bool kStoredInPlace = sizeof(Callable) <= kInlineBytes &&
                                           alignof(Callable) <= alignof(std::max_align_t) &&
                                           std::is_nothrow_move_constructible_v<Callable>;

    template <typename Callable>
    static Callable& InPlace(void* storage) noexcept
    {
        return *std::launder(reinterpret_cast<Callable*>(storage));
    }

    template <typename Callable>
    static Callable*& OnHeap(void* storage) noexcept
    {
        return *std::launder(reinterpret_cast<Callable**>(storage));
    }

    template <typename Callable>
    static Result InvokeInPlace(void* storage, Arguments&&... arguments)
    {
        return static_cast<Result>(InPlace<Callable>(storage)(std::forward<Arguments>(arguments)...));
    }

    template <typename Callable>
    static void RelocateInPlace(void* from, void* to) noexcept
    {
        Callable& source = InPlace<Callable>(from);
        ::new (to) Callable(std::move(source));
        source.~Callable();
    }

    template <typename Callable>
    static void DestroyInPlace(void* storage) noexcept
    {
        InPlace<Callable>(storage).~Callable();
    }

    template <typename Callable>
    static Result InvokeOnHeap(void* storage, Arguments&&... arguments)
    {
        return static_cast<Result>((*OnHeap<Callable>(storage))(std::forward<Arguments>(arguments)...));
    }

    template <typename Callable>
    static void RelocateOnHeap(void* from, void* to) noexcept
    {
        ::new (to) Callable*(OnHeap<Callable>(from));
    }

    template <typename Callable>
    static void DestroyOnHeap(void* storage) noexcept
    {
        delete OnHeap<Callable>(storage);
    }

    template <typename Callable>
    static constexpr Operations kInPlaceOperations{&InvokeInPlace<Callable>, &RelocateInPlace<Callable>,
                                                   &DestroyInPlace<Callable>};

    template <typename Callable>
    static constexpr Operations kOnHeapOperations{&InvokeOnHeap<Callable>, &RelocateOnHeap<Callable>,
                                                  &DestroyOnHeap<Callable>};

    template <typename Callable>
    void Store(Callable&& callable)
    {
        using Stored = std::decay_t<Callable>;
        // A function pointer and a std::function can be empty; a lambda that converts to bool is never empty.
        if constexpr (std::is_constructible_v<bool, const Stored&>)
        {
            if (!static_cast<bool>(callable))
                return;
        }

        if constexpr (kStoredInPlace<Stored>)
        {
            ::new (static_cast<void*>(m_Storage)) Stored(std::forward<Callable>(callable));
            m_Operations = &kInPlaceOperations<Stored>;
        }
        else
        {
            ::new (static_cast<void*>(m_Storage)) Stored*(new Stored(std::forward<Callable>(callable)));
            m_Operations = &kOnHeapOperations<Stored>;
        }
    }

    void TakeFrom(MoveOnlyFunction& other) noexcept
    {
        if (!other.m_Operations)
            return;
        other.m_Operations->Relocate(other.m_Storage, m_Storage);
        m_Operations = other.m_Operations;
        other.m_Operations = nullptr;
    }

    void Reset() noexcept
    {
        if (!m_Operations)
            return;
        m_Operations->Destroy(m_Storage);
        m_Operations = nullptr;
    }

    const Operations* m_Operations = nullptr;
    alignas(std::max_align_t) mutable std::byte m_Storage[kInlineBytes];
};

} // namespace GameEngine
