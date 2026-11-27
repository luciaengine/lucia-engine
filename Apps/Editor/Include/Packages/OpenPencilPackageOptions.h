#pragma once

#include <array>

namespace GameEngine::Editor
{
inline constexpr char kOpenPencilPackageId[] = "open-pencil";

struct OpenPencilPackageOption
{
    const char* CheckboxId;
    const char* Setting;
    const char* PanelId;
    const char* PanelType;
};

inline constexpr std::array kOpenPencilPackageOptions{
    OpenPencilPackageOption{"OpenPencilOfflineOption", "packages.open-pencil.offline",
                            "OpenPencilLocal", "LocalOpenPencilPanel"},
    OpenPencilPackageOption{"OpenPencilOnlineOption", "packages.open-pencil.online",
                            "OpenPencil", "OpenPencilPanel"},
};
}
