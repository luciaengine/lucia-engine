#pragma once

#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>
#include <type_traits>
#include <cstring>

namespace GameEngine {
namespace Rendering {

    /**
     * @brief Enhanced Specialization Constants Manager
     * 
     * Provides type-safe, high-performance specialization constant management
     * for Vulkan shaders. Reduces shader variants and improves performance.
     * 
     * Features:
     * - Type-safe constant definition
     * - Automatic layout management
     * - Shader variant optimization
     * - Template-based API for ease of use
     */
    class SpecializationConstants {
    public:
        /**
         * @brief Specialization constant entry
         */
        struct Entry {
            uint32_t ConstantId;
            uint32_t Offset;
            size_t Size;
            std::string Name; // For debugging
        };

        SpecializationConstants() = default;
        ~SpecializationConstants() = default;

        /**
         * @brief Add a specialization constant
         * @tparam T Type of the constant (must be POD)
         * @param constantId Shader constant ID
         * @param value Initial value
         * @param name Debug name for the constant
         */
        template<typename T>
        void AddConstant(uint32_t constantId, const T& value, const std::string& name = "") {
            static_assert(std::is_trivially_copyable_v<T>, "Specialization constants must be trivially copyable");
            
            Entry entry;
            entry.ConstantId = constantId;
            entry.Offset = static_cast<uint32_t>(m_Data.size());
            entry.Size = sizeof(T);
            entry.Name = name.empty() ? ("Constant_" + std::to_string(constantId)) : name;

            // Add to data buffer
            size_t oldSize = m_Data.size();
            m_Data.resize(oldSize + sizeof(T));
            std::memcpy(m_Data.data() + oldSize, &value, sizeof(T));

            // Store entry
            m_Entries[constantId] = entry;
        }

        // Backend-agnostic accessors should be implemented in backend-specific layers.

        /**
         * @brief Check if there are any constants defined
         */
        bool IsEmpty() const { return m_Entries.empty(); }

        /**
         * @brief Get total data size
         */
        size_t GetDataSize() const { return m_Data.size(); }

        /**
         * @brief Get raw data pointer (may be null if empty)
         */
        const uint8_t* GetDataPtr() const { return m_Data.empty() ? nullptr : m_Data.data(); }

        /**
         * @brief Get all constant entries (for debugging)
         */
        const std::unordered_map<uint32_t, Entry>& GetEntries() const { return m_Entries; }

        /**
         * @brief Generate a hash for shader variant management
         */
        uint64_t GetHash() const;

    private:
        std::unordered_map<uint32_t, Entry> m_Entries;
        std::vector<uint8_t> m_Data;
    };

} // namespace Rendering
} // namespace GameEngine
