#pragma once

#include "Types/Types.h"

#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace GameEngine::Rendering
{
struct SortingLayer
{
    int32 Id = 0;
    std::string Name;
    int32 Order = 0;
    bool operator==(const SortingLayer&) const = default;
};

/// Stable layer IDs with independently editable display names and draw priorities.
/// Editor settings and packaged game.config carry the same sortingLayers array.
struct SortingLayers
{
    std::vector<SortingLayer> Layers;
    static SortingLayers Defaults();
    static SortingLayers Current();
    static uint64 Revision();
    static bool SetCurrent(const SortingLayers& layers, std::string& error);
    bool Validate(std::string& error) const;
    int32 Priority(int32 id) const;
    static SortingLayers FromJson(const nlohmann::json& root);
    void WriteToJson(nlohmann::json& root) const;
    static SortingLayers Load(const std::filesystem::path& projectRoot);
};
}
