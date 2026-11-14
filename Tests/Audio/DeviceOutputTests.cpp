#include <gtest/gtest.h>

#include "DeviceOutput.h"
#include "Assets/AssetManager.h"

#include <array>
#include <atomic>
#include <vector>

using GameEngine::Audio::Detail::FinalizeDeviceOutput;

TEST(AudioDeviceOutput, MutedSpeakersDoNotMuteRecordingCapture)
{
    std::array<float, 8> output{0.25f, -0.5f, 0.75f, -0.125f, 0.0f, 0.1f, -0.2f, 0.4f};
    const auto mixed = output;
    std::vector<float> captured;
    std::atomic<bool> muted{true};
    const float peak = FinalizeDeviceOutput(output.data(), 4, 2, 48000, muted,
        [&](const float* samples, std::uint32_t frames, std::uint32_t channels, std::uint32_t rate)
        {
            EXPECT_EQ(frames, 4u);
            EXPECT_EQ(channels, 2u);
            EXPECT_EQ(rate, 48000u);
            captured.assign(samples, samples + frames * channels);
        });
    EXPECT_EQ(captured, (std::vector<float>(mixed.begin(), mixed.end())));
    EXPECT_EQ(output, (std::array<float, 8>{}));
    EXPECT_FLOAT_EQ(peak, 0.0f);
}

TEST(AudioDeviceOutput, EnabledSpeakersPreserveTheMixAndItsPeak)
{
    std::array<float, 4> output{0.25f, -0.75f, 0.5f, -0.125f};
    const auto mixed = output;
    std::atomic<bool> muted{false};
    int captures = 0;
    const float peak = FinalizeDeviceOutput(output.data(), 2, 2, 44100, muted,
        [&](const float* samples, std::uint32_t, std::uint32_t, std::uint32_t)
        {
            ++captures;
            EXPECT_EQ(samples, output.data());
        });
    EXPECT_EQ(output, mixed);
    EXPECT_EQ(captures, 1);
    EXPECT_FLOAT_EQ(peak, 0.75f);
}

TEST(AudioDeviceOutput, UnmutingUsesTheNextMixInsteadOfRestoringOldSamples)
{
    std::atomic<bool> muted{true};
    std::array<float, 2> output{0.5f, -0.25f};
    EXPECT_FLOAT_EQ(FinalizeDeviceOutput(output.data(), 2, 1, 48000, muted, {}), 0.0f);
    output = {0.125f, -0.375f};
    muted.store(false);
    EXPECT_FLOAT_EQ(FinalizeDeviceOutput(output.data(), 2, 1, 48000, muted, {}), 0.375f);
    EXPECT_EQ(output, (std::array<float, 2>{0.125f, -0.375f}));
}

TEST(AudioDeviceOutput, TheDeviceMuteIsReadAfterTheCaptureTap)
{
    std::atomic<bool> muted{false};
    std::array<float, 2> output{0.5f, -0.25f};
    const float peak = FinalizeDeviceOutput(output.data(), 2, 1, 48000, muted,
        [&](const float* samples, std::uint32_t, std::uint32_t, std::uint32_t)
        {
            EXPECT_FLOAT_EQ(samples[0], 0.5f);
            muted.store(true);
        });
    EXPECT_EQ(output, (std::array<float, 2>{}));
    EXPECT_FLOAT_EQ(peak, 0.0f);
}

TEST(AudioDeviceOutput, InvalidOutputDoesNotInvokeCapture)
{
    std::atomic<bool> muted{true};
    float sample = 0.5f;
    int captures = 0;
    const GameEngine::Audio::AudioSystem::OutputCaptureCallback capture =
        [&](const float*, std::uint32_t, std::uint32_t, std::uint32_t) { ++captures; };
    EXPECT_FLOAT_EQ(FinalizeDeviceOutput(nullptr, 1, 1, 48000, muted, capture), 0.0f);
    EXPECT_FLOAT_EQ(FinalizeDeviceOutput(&sample, 1, 0, 48000, muted, capture), 0.0f);
    EXPECT_EQ(captures, 0);
    EXPECT_FLOAT_EQ(sample, 0.5f);
}

TEST(AudioMasterMute, APreInitializationChoiceSurvivesShutdown)
{
    GameEngine::AssetManager assets;
    GameEngine::Audio::AudioSystem audio(assets);
    EXPECT_FALSE(audio.IsMasterMuted());
    audio.SetMasterMuted(true);
    audio.Shutdown();
    EXPECT_TRUE(audio.IsMasterMuted());
    EXPECT_FALSE(audio.IsInitialized());
    audio.SetMasterMuted(false);
    EXPECT_FALSE(audio.IsMasterMuted());
}
