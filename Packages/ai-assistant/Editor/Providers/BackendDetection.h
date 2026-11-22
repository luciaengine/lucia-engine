#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
/// Finds the assistant programs a user installed, so the AI Assistant works without setup when
/// claude or codex is installed in the usual place. An editor opened from the desktop often has a
/// shorter PATH than a terminal (on macOS it lacks Homebrew's folder), so a program is also looked
/// for in the folders its installers use.
namespace BackendDetection
{
/// Where a search looks: the PATH value and the user's home folder.
struct Inputs
{
    std::string Path;
    std::filesystem::path Home;
    /// Whether the folders outside the home folder (Homebrew, /usr/local/bin, /Applications) are
    /// searched; a test turns them off so the machine's own programs do not count.
    bool SystemFolders = true;
};

/// Where a program was found.
struct Found
{
    /// The program's full path.
    std::filesystem::path Program;
    /// True when a folder on Inputs::Path holds it, so its bare name starts it.
    bool OnPath = false;
};

/// The PATH and home of this process.
Inputs CurrentInputs();

/// The folders `program` is looked for in after PATH, in order: the user's local bin folder (the
/// native installers), Claude Code's own local folder (claude only), Homebrew, /usr/local/bin and
/// the npm and Bun global folders. Windows has the user's local bin folder only. `dsh` is looked
/// for in DeepSeek Harness's app bundle instead (~/Applications, then /Applications, on macOS).
std::vector<std::filesystem::path> InstallFolders(std::string_view program, const Inputs& inputs);

/// `program` (claude, codex) on PATH, else in InstallFolders(); nullopt when it is in neither.
/// A match is a regular file, executable on POSIX; Windows appends ".exe".
std::optional<Found> FindProgram(std::string_view program, const Inputs& inputs);
} // namespace BackendDetection
} // namespace GameEngine
