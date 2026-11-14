#pragma once

#include "Events/Event.h"

namespace GameEngine::Editor
{

// Seed the Asset Preview runtime state (scroll-wheel dolly) from
// Preferences.json. Called once at editor startup, before any Asset View
// panel consumes the value.
void ApplyAssetPreviewSettingsFromPreferences();

// Register the Asset Preview settings category (UI > Asset Preview).
void RegisterAssetPreviewSettingsCategory();

bool GetPackagePreviewsEnabled();
// Main-thread notification. Package panels rebuild after the settings event.
Event<>& PackagePreviewsChanged();

} // namespace GameEngine::Editor
