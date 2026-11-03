#include "TestTempDir.h"
#include "../../Engine/Modules/Rendering/Tests/ScopedEnvVar.h"

#include <gtest/gtest.h>

namespace
{
// Shared SDK controls can read preferences during construction. Keep every test's default
// root empty and owned by this process; a case can still install its own scoped preferences.
class EditorPreferencesEnvironment final : public testing::Environment
{
public:
    EditorPreferencesEnvironment()
        : m_Root(GameEngine::TestUtils::MakeUniqueTempDirectory("editor-test-preferences"))
        , m_Override("GE_EDITOR_USER_DATA_ROOT", m_Root.Path().string().c_str())
    {
    }

private:
    GameEngine::TestUtils::ScopedTempDir m_Root;
    GameEngine::Rendering::Tests::ScopedEnvVar m_Override;
};

[[maybe_unused]] const auto* const g_Preferences =
    testing::AddGlobalTestEnvironment(new EditorPreferencesEnvironment());
}
