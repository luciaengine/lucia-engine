// The Time of Day row moves the sun even when the sun was set by hand.

#include <gtest/gtest.h>

#include "Components/Rendering/SkyEnvironment.h"
#include "Sky/SkyEnvironment/SkyEnvironmentEdit.h"

using namespace GameEngine;

// A hand-set sun (Auto Sun/Moon off) ignores the hour. Setting the hour turns Auto Sun/Moon on, so
// the hour places the sun, and keeps the hand-set direction for when it is turned off again.
TEST(SkyTimeOfDay, SettingTheHourLetsItPlaceAHandSetSun)
{
    Components::SkyEnvironment sky{};
    sky.AutoSunMoon = false;
    sky.SunDirOverride[0] = 0.25f;
    sky.SunDirOverride[1] = 0.5f;
    sky.SunDirOverride[2] = 0.75f;

    Editor::SetSkyHourAndPlaceSun(sky, 17.5f);

    EXPECT_FLOAT_EQ(sky.TimeOfDayHours, 17.5f);
    EXPECT_TRUE(sky.AutoSunMoon) << "the hour does not place a hand-set sun";
    EXPECT_FLOAT_EQ(sky.SunDirOverride[0], 0.25f) << "the hand-set direction was lost";
    EXPECT_FLOAT_EQ(sky.SunDirOverride[1], 0.5f);
    EXPECT_FLOAT_EQ(sky.SunDirOverride[2], 0.75f);
}
