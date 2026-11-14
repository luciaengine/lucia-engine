#pragma once

#include "ECS/ComponentConcepts.h"

namespace GameEngine::ECS
{

template <QualifiedComponent... Ts>
class Query;

// One row of a Write<T> column, as Query::Each and Query::Parallel pass it.
// Reading is free. Mutable() and Set() record that the callback wrote the
// row; the query stamps the column of every chunk with a recorded write when
// it finishes that chunk, so Changed<T> consumers see the chunks a writer
// changed and skip the chunks it only visited.
//
// Take it by value: it is a pointer to the row plus a pointer to the chunk's
// written flag. The grant covers the row for the whole dispatch, so a system
// may keep the reference Mutable() returned and write through it later in the
// same update, before any Changed<T> consumer of the column runs.
template <typename T>
class Writable
{
  public:
    const T& Get() const { return *m_Value; }
    const T* operator->() const { return m_Value; }

    // Grants the write and returns the row for in-place mutation.
    T& Mutable()
    {
        *m_Written = true;
        return *m_Value;
    }

    // Grants the write and assigns the row.
    void Set(const T& value)
    {
        *m_Written = true;
        *m_Value = value;
    }

  private:
    template <QualifiedComponent... Ts>
    friend class Query;

    Writable(T* value, bool* written) : m_Value(value), m_Written(written) {}

    T* m_Value;
    bool* m_Written;
};

} // namespace GameEngine::ECS
