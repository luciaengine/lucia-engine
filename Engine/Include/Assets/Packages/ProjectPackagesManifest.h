#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GameEngine
{

// Parsed <project>/Packages/manifest.json.
//
//   {
//     "dependencies": {
//       "ocean-pack": "file:../SharedPackages/OceanPack",   // local path
//       "debug-tools": "embedded"                           // <project>/Packages/debug-tools/
//     },
//     "disabled": ["heavy-megapack"],
//     "enabled": ["optional-baker"]
//   }
//
// P0 supports the "file:<path>" and "embedded" specs only (git+... is the P3
// distribution slice). A missing manifest file means zero packages, zero noise.
struct ProjectPackagesManifest
{
    // Dependency name -> raw spec text ("file:<path>" or "embedded").
    std::map<std::string, std::string> Dependencies;
    std::set<std::string> Disabled;
    // Explicit opt-in for staged packages whose enabledByDefault is false.
    std::set<std::string> Enabled;
};

// Load <manifestFile>. Returns:
//  - true with an empty manifest when the file does not exist (the quiet path),
//  - true with parsed contents on success,
//  - false with outError set when the file exists but is malformed.
bool TryLoadProjectPackagesManifest(const std::filesystem::path& manifestFile,
                                    ProjectPackagesManifest& out,
                                    std::string& outError);

// Update all package names in one manifest read/write, preserving unrelated fields.
// Names are validated before editing; enable records explicit opt-ins. Mounts update on reopen.
bool SetPackageDisabledInProjectManifest(const std::filesystem::path& manifestFile,
                                         const std::vector<std::string>& packageNames,
                                         bool disabled,
                                         std::string& outError);

// Append "dependencies.<packageName>": <spec>, creating the manifest file
// (and parent directories) when it does not exist yet. Fails when the name is
// invalid, the dependency is already declared, or the file is malformed —
// spec-level validation (path exists / git URL shape) is the Add flow's job
// (PackageManagementActions.h), not this JSON edit's.
bool AddPackageToProjectManifest(const std::filesystem::path& manifestFile,
                                 const std::string& packageName,
                                 const std::string& spec,
                                 std::string& outError);

// Delete "dependencies.<packageName>" and matching enabled/disabled overrides.
// Fails when the dependency is not declared (the caller is confused about
// state — surface it) or the file is missing/malformed.
bool RemovePackageFromProjectManifest(const std::filesystem::path& manifestFile,
                                      const std::string& packageName,
                                      std::string& outError);

// Rewrite "dependencies.<packageName>" to `newSpec` (re-pin a git ref,
// relocate a file: path). Fails when the dependency is not declared.
bool SetPackageSpecInProjectManifest(const std::filesystem::path& manifestFile,
                                     const std::string& packageName,
                                     const std::string& newSpec,
                                     std::string& outError);

} // namespace GameEngine
