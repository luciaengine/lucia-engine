/**
 * @file SpecializationConstantsTest.cpp
 * @brief Unit tests for Specialization Constants functionality
 */

#include <gtest/gtest.h>
#include "Rendering/Core/SpecializationConstants.h"

#include <cstring>
#include <memory>

using namespace GameEngine::Rendering;

class SpecializationConstantsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Fresh constants for each test
        m_Constants = std::make_unique<SpecializationConstants>();
    }

    void TearDown() override {
        m_Constants.reset();
    }

    std::unique_ptr<SpecializationConstants> m_Constants;
};

TEST_F(SpecializationConstantsTest, BasicConstantAddition) {
    // Test adding different types of constants
    m_Constants->AddConstant(0, 42u, "TestUInt");
    m_Constants->AddConstant(1, 3.14f, "TestFloat");
    m_Constants->AddConstant(2, true, "TestBool");

    EXPECT_EQ(m_Constants->GetEntries().size(), 3u);
    EXPECT_FALSE(m_Constants->IsEmpty());
}

// Backends build their native specialization info from GetEntries() and
// GetDataPtr(): each entry's Offset and Size must address its value in the
// packed data.
TEST_F(SpecializationConstantsTest, EntriesAddressTheirValuesInPackedData) {
    m_Constants->AddConstant(0, 32u);
    m_Constants->AddConstant(1, true);
    m_Constants->AddConstant(2, 1.5f);

    ASSERT_EQ(m_Constants->GetEntries().size(), 3u);
    ASSERT_NE(m_Constants->GetDataPtr(), nullptr);
    EXPECT_EQ(m_Constants->GetDataSize(), sizeof(uint32_t) + sizeof(bool) + sizeof(float));

    for (const auto& [constantId, entry] : m_Constants->GetEntries()) {
        EXPECT_EQ(entry.ConstantId, constantId);
        EXPECT_LE(entry.Offset + entry.Size, m_Constants->GetDataSize());
    }

    const auto& entries = m_Constants->GetEntries();
    const uint8_t* data = m_Constants->GetDataPtr();
    uint32_t uintValue = 0;
    bool boolValue = false;
    float floatValue = 0.0f;
    std::memcpy(&uintValue, data + entries.at(0).Offset, entries.at(0).Size);
    std::memcpy(&boolValue, data + entries.at(1).Offset, entries.at(1).Size);
    std::memcpy(&floatValue, data + entries.at(2).Offset, entries.at(2).Size);
    EXPECT_EQ(uintValue, 32u);
    EXPECT_TRUE(boolValue);
    EXPECT_FLOAT_EQ(floatValue, 1.5f);
}

TEST_F(SpecializationConstantsTest, HashGeneration) {
    m_Constants->AddConstant(0, 100u);
    m_Constants->AddConstant(1, 200u);
    
    uint64_t hash1 = m_Constants->GetHash();
    EXPECT_NE(hash1, 0u);

    // Same constants should produce same hash
    SpecializationConstants identical;
    identical.AddConstant(0, 100u);
    identical.AddConstant(1, 200u);
    
    uint64_t hash2 = identical.GetHash();
    EXPECT_EQ(hash1, hash2);

    // Different constants should produce different hash
    SpecializationConstants different;
    different.AddConstant(0, 101u); // Different value
    different.AddConstant(1, 200u);
    
    uint64_t hash3 = different.GetHash();
    EXPECT_NE(hash1, hash3);
}
