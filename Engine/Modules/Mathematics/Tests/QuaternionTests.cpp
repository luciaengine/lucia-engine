#include "Mathematics/Quaternion.h"

#include <gtest/gtest.h>

#include <numbers>

using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

namespace
{

constexpr float kTolerance = 1.0e-5f;

void ExpectSameRotation(const Quaternion& actual, const Quaternion& expected)
{
    const Vector3 probe(1.0f, 0.5f, -0.25f);
    const Vector3 actualRotated = actual.Rotate(probe);
    const Vector3 expectedRotated = expected.Rotate(probe);
    EXPECT_NEAR(actualRotated.x, expectedRotated.x, kTolerance);
    EXPECT_NEAR(actualRotated.y, expectedRotated.y, kTolerance);
    EXPECT_NEAR(actualRotated.z, expectedRotated.z, kTolerance);
}

} // namespace

TEST(QuaternionTests, SlerpTakesTheShortPathAndReturnsTheEndpoints)
{
    const Vector3 up(0.0f, 1.0f, 0.0f);
    const float quarterTurn = std::numbers::pi_v<float> * 0.5f;
    const Quaternion start = Quaternion::FromAxisAngle(up, 0.0f);
    const Quaternion end = Quaternion::FromAxisAngle(up, quarterTurn);
    const glm::quat endGlm = end.GetGLM();
    const Quaternion endNegated(-endGlm.w, -endGlm.x, -endGlm.y, -endGlm.z);

    const Quaternion eighthTurn = Quaternion::FromAxisAngle(up, quarterTurn * 0.5f);
    ExpectSameRotation(Quaternion::Slerp(start, end, 0.5f), eighthTurn);
    ExpectSameRotation(Quaternion::Slerp(start, endNegated, 0.5f), eighthTurn);

    ExpectSameRotation(Quaternion::Slerp(start, end, 0.0f), start);
    ExpectSameRotation(Quaternion::Slerp(start, end, 1.0f), end);
    ExpectSameRotation(Quaternion::Slerp(start, end, 1.5f), end);
    ExpectSameRotation(Quaternion::Slerp(start, end, -0.5f), start);
    EXPECT_NEAR(glm::length(Quaternion::Slerp(start, endNegated, 1.0f).GetGLM()), 1.0f, kTolerance);
}
