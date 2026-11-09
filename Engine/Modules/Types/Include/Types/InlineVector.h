#pragma once

#include "Types/GeometricReserve.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace GameEngine
{

/// A vector that keeps its first `N` elements inside the object. Up to `N` elements it never
/// allocates; past `N` it moves its elements into one heap block, taken through the global
/// `operator new` so the allocation counter sees it. `PushBack` grows the block geometrically
/// (`NextGeometricCapacity`): the first spill holds `2N` elements (8 when `N` is less than 4),
/// and each later block doubles, up to 2^32 - 1 elements.
///
/// It suits a member or a local whose element count is small in the common case: the sector
/// keys of one entity, the residents of one descriptor set. Choose `N` from a measured count:
/// every instance carries `N * sizeof(T)` bytes whether it uses them or not.
///
/// `T` is trivially copyable and needs no more than the default `operator new` alignment. Elements
/// are copied with `memcpy` and never destroyed. The vector holds no pointer into itself.
///
/// A function that reads or writes the elements takes `std::span<const T>` (or `std::span<T>`),
/// which an InlineVector converts to implicitly, so `N` stays the owner's choice. Only a function
/// that fills the owner's vector, reusing its storage, takes the vector itself. Growing, moving
/// and destroying the vector invalidate pointers and spans into it.
///
/// Moving leaves the source empty. Debug and DebugFast assert on an index past `Size()` and on
/// reading the elements of a vector that holds a moved-from value; `Clear`, `PushBack` and
/// `Resize` give it a value again, and assignment gives it the source's.
template <typename T, std::size_t N>
    requires std::is_trivially_copyable_v<T> && (alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) && (N > 0)
class InlineVector
{
  public:
    InlineVector() noexcept = default;
    ~InlineVector() { ReleaseHeap(); }

    InlineVector(const InlineVector& other) { CopyElementsFrom(other); }
    InlineVector(InlineVector&& other) noexcept { TakeElementsFrom(other); }

    /// Reuses this vector's storage when the source's elements fit in it.
    InlineVector& operator=(const InlineVector& other)
    {
        if (this != &other)
        {
            m_Size = 0;
            CopyElementsFrom(other);
        }
        return *this;
    }

    /// Releases this vector's heap block, then takes the source's block or copies its inline
    /// elements, as construction does.
    InlineVector& operator=(InlineVector&& other) noexcept
    {
        if (this != &other)
        {
            ReleaseHeap();
            TakeElementsFrom(other);
        }
        return *this;
    }

    std::size_t Size() const noexcept { return m_Size; }
    bool Empty() const noexcept { return m_Size == 0; }

    T* begin() noexcept { return Elements(); }
    T* end() noexcept { return Elements() + m_Size; }
    const T* begin() const noexcept { return Elements(); }
    const T* end() const noexcept { return Elements() + m_Size; }

    T& operator[](std::size_t index) noexcept
    {
        assert(index < m_Size && "InlineVector index past Size()");
        return Elements()[index];
    }

    const T& operator[](std::size_t index) const noexcept
    {
        assert(index < m_Size && "InlineVector index past Size()");
        return Elements()[index];
    }

    void PushBack(const T& value)
    {
        static_assert(offsetof(InlineVector, m_HeapCapacity) > offsetof(InlineVector, m_Size) + sizeof(m_Size),
                      "m_Heap sits between m_Size and m_HeapCapacity; side by side they stall every PushBack");
        MarkHoldsAValue();
        if (m_Size == Capacity())
        {
            // `value` may be one of this vector's elements; copy it before the block moves.
            const T copy = value;
            Grow(CapacityForOneMore());
            Storage()[m_Size++] = copy;
            return;
        }
        Storage()[m_Size++] = value;
    }

    /// Shrinks to `count`, or grows to `count` filling the new elements with `value`. Growing
    /// past the capacity allocates a block of exactly `count`.
    void Resize(std::size_t count, const T& value)
    {
        MarkHoldsAValue();
        if (count > Capacity())
        {
            const T copy = value;
            Grow(count);
            FillTo(count, copy);
            return;
        }
        FillTo(count, value);
    }

    /// Allocates a block of exactly `capacity` when the current storage is smaller.
    void Reserve(std::size_t capacity)
    {
        if (capacity > Capacity())
            Grow(capacity);
    }

    /// Keeps the storage, inline or heap.
    void Clear() noexcept
    {
        MarkHoldsAValue();
        m_Size = 0;
    }

  private:
    std::size_t Capacity() const noexcept { return m_Heap != nullptr ? m_HeapCapacity : N; }

    T* Storage() noexcept { return m_Heap != nullptr ? m_Heap : reinterpret_cast<T*>(m_Inline); }
    const T* Storage() const noexcept
    {
        return m_Heap != nullptr ? m_Heap : reinterpret_cast<const T*>(m_Inline);
    }

    /// The storage, for a read of the elements: asserts that the vector holds a value.
    T* Elements() noexcept
    {
        AssertHoldsAValue();
        return Storage();
    }

    const T* Elements() const noexcept
    {
        AssertHoldsAValue();
        return Storage();
    }

    void FillTo(std::size_t count, const T& value) noexcept
    {
        T* storage = Storage();
        for (std::size_t index = m_Size; index < count; ++index)
            storage[index] = value;
        m_Size = static_cast<std::uint32_t>(count);
    }

    /// The capacity `PushBack` grows a full vector to: the next geometric step, computed in
    /// `std::size_t` so it cannot wrap. A vector that already holds `kMaxCapacity` elements asks
    /// for one more than that, which `Grow` refuses.
    std::size_t CapacityForOneMore() const noexcept
    {
        return std::max(std::size_t{m_Size} + 1, NextGeometricCapacity(m_Size, kMaxCapacity));
    }

    /// Moves the elements into a new heap block of at least `minimumCapacity` (and never less
    /// than `N`). The new block is taken before anything changes, so a throw leaves the vector
    /// as it was.
    void Grow(std::size_t minimumCapacity)
    {
        const std::size_t capacity = minimumCapacity > N ? minimumCapacity : N;
        if (capacity > kMaxCapacity)
            throw std::length_error("InlineVector capacity past 2^32 - 1 elements");
        T* block = Allocate(capacity);
        std::memcpy(block, Storage(), m_Size * sizeof(T));
        ReleaseHeap();
        m_Heap = block;
        m_HeapCapacity = static_cast<std::uint32_t>(capacity);
    }

    static T* Allocate(std::size_t capacity)
    {
        return static_cast<T*>(::operator new(capacity * sizeof(T)));
    }

    void ReleaseHeap() noexcept
    {
        if (m_Heap == nullptr)
            return;
        ::operator delete(m_Heap);
        m_Heap = nullptr;
        m_HeapCapacity = 0;
    }

    /// Appends `other`'s elements to this vector's (emptied) storage, growing it to exactly
    /// their count when they do not fit.
    void CopyElementsFrom(const InlineVector& other)
    {
        if (other.m_Size > Capacity())
            Grow(other.m_Size);
        std::memcpy(Storage(), other.Storage(), other.m_Size * sizeof(T));
        m_Size = other.m_Size;
        CopyMovedFromState(other);
    }

    /// Construction and move assignment from `other`, into a vector without a heap block: takes
    /// `other`'s block when it has one, else copies its inline elements. Leaves `other` empty and
    /// inline.
    void TakeElementsFrom(InlineVector& other) noexcept
    {
        if (other.m_Heap != nullptr)
        {
            m_Heap = other.m_Heap;
            m_HeapCapacity = other.m_HeapCapacity;
            other.m_Heap = nullptr;
            other.m_HeapCapacity = 0;
        }
        else
        {
            std::memcpy(m_Inline, other.m_Inline, other.m_Size * sizeof(T));
        }
        m_Size = other.m_Size;
        CopyMovedFromState(other);
        other.LeaveMovedFrom();
    }

    void LeaveMovedFrom() noexcept
    {
        m_Size = 0;
#if GE_DEBUG_INSTRUMENTATION
        m_MovedFrom = true;
#endif
    }

    void CopyMovedFromState([[maybe_unused]] const InlineVector& other) noexcept
    {
#if GE_DEBUG_INSTRUMENTATION
        m_MovedFrom = other.m_MovedFrom;
#endif
    }

    void MarkHoldsAValue() noexcept
    {
#if GE_DEBUG_INSTRUMENTATION
        m_MovedFrom = false;
#endif
    }

    void AssertHoldsAValue() const noexcept
    {
#if GE_DEBUG_INSTRUMENTATION
        assert(!m_MovedFrom && "InlineVector read after it was moved from; assign, Clear, PushBack or Resize first");
#endif
    }

    static constexpr std::size_t kMaxCapacity = UINT32_MAX;

    // m_Inline is first, so its alignment adds no padding (MSVC C4324). m_Heap keeps m_Size apart from
    // m_HeapCapacity: side by side, arm64 reads both in one paired load, which waits for the store of
    // m_Size from the previous PushBack.
    alignas(T) std::byte m_Inline[N * sizeof(T)];
    std::uint32_t m_Size = 0;
    T* m_Heap = nullptr;
    std::uint32_t m_HeapCapacity = 0;
#if GE_DEBUG_INSTRUMENTATION
    /// The vector holds a moved-from value: its elements are not to be read.
    bool m_MovedFrom = false;
#endif
};

} // namespace GameEngine
