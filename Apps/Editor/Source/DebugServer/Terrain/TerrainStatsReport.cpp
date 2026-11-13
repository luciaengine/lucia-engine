#include "DebugServer/Terrain/TerrainStatsReport.h"

#include "CBTTerrain/CBTScreenTarget.h"
#include "TerrainECS/TerrainPages.h"
#include "Types/FormatNumber.h"

#include <string>

namespace GameEngine::Editor
{

namespace
{
// Significant digits of the targets in the summary line.
constexpr int kSummaryDigits = 6;
} // namespace

nlohmann::json DescribeTerrainSplitThreshold(float targetPixelError, float appliedThresholdPx, uint32 renderHeightPx)
{
    nlohmann::json out;
    out["targetPixelError"] = targetPixelError;
    out["targetReferenceHeightPx"] = CBTTerrain::kTargetReferenceHeightPx;
    const std::string target = "(target " + FormatFloat(targetPixelError, kSummaryDigits) + " px at " +
                               FormatFloat(CBTTerrain::kTargetReferenceHeightPx, kSummaryDigits) + " rows).";
    if (renderHeightPx == 0u)
    {
        out["splitThresholdPx"] = nullptr;
        out["renderHeightPx"] = nullptr;
        out["summary"] = "Split threshold: no frame built yet " + target;
        return out;
    }
    out["splitThresholdPx"] = appliedThresholdPx;
    out["renderHeightPx"] = renderHeightPx;
    out["summary"] = "Split threshold: " + FormatFixed(appliedThresholdPx, 2) + " px at " +
                     std::to_string(renderHeightPx) + " render rows " + target;
    return out;
}

nlohmann::json DescribePlanetPageTable(const TerrainECS::PageStreamCounts& stream)
{
    nlohmann::json faces = nlohmann::json::array();
    for (const TerrainECS::HeightPageFaceTable& face : stream.Faces)
        faces.push_back({{"entriesPerLevel", face.EntriesPerLevel}, {"resolvedLevel", face.ResolvedLevel}});
    return {{"published", stream.Published},
            {"pinnedLevel", stream.PinnedLevel},
            {"pinnedPagesX", stream.PinnedPagesX},
            {"pinnedPagesZ", stream.PinnedPagesZ},
            {"faces", std::move(faces)}};
}

} // namespace GameEngine::Editor
