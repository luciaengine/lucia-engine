#include "Assets/Parsers/AnimationAssetParser.h"

#include "Animation/Compression/AnimationClipArchive.h"

#include <fstream>
#include <iterator>

namespace GameEngine
{

bool AnimationAssetParser::CookForPackage(const std::filesystem::path& stagedFile, std::string& error) const
{
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(stagedFile, sizeError);
    if (sizeError || size > Animation::AnimationClipArchive::MaximumEncodedBytes())
    {
        error = "Could not read the animation clip within the 256 MiB source limit.";
        return false;
    }
    std::ifstream input(stagedFile, std::ios::binary);
    if (!input)
    {
        error = "Could not read the staged animation clip.";
        return false;
    }
    Vector<uint8> source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad() || source.size() != size)
    {
        error = "Could not read the complete staged animation clip.";
        return false;
    }
    input.close();
    AnimationClip clip(GUID::Generate(), stagedFile);
    if (!clip.LoadFromData(source))
    {
        error = "The staged animation clip is invalid. Repair its source and export again.";
        return false;
    }
    if (Animation::AnimationClipArchive::IsArchive(source))
        return true;
    Vector<uint8> encoded;
    if (!Animation::AnimationClipArchive::Encode(source, encoded))
    {
        error = "The animation clip exceeds the 256 MiB source limit or could not be compressed.";
        return false;
    }
    std::ofstream output(stagedFile, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    output.close();
    if (!output)
    {
        error = "Could not write the packaged animation clip.";
        return false;
    }
    return true;
}

// AnimationClip's serializer (AnimationClip::SaveToData) emits version,
// duration, selectedAnimationIndex, source { path, animationIndex }, and
// channels[]. None of those are GUID-form references to other assets that
// belong in the runtime dep graph — channels reference bone names by string
// (not assets); `source.path` records which FBX/glTF the clip was extracted
// from, which is import-time provenance, not a runtime dependency.
//
// Returning true with no edges suppresses the registry's syntactic fallback
// (which would otherwise regex-scan the JSON and emit spurious "Other" edges
// for any GUID-shaped strings it stumbled on). If a future authoring side
// embeds GUID-form references into .anim content (e.g. a humanoid retarget
// rig pin), revisit this method to walk those fields explicitly.
//
// If/when we want producer/sidecar tracking ("which FBX produced this
// clip?"), use the m_AssetProvenance mechanism rather than overloading the
// dep graph — same shape as the model -> material sidecar relationship.
bool AnimationAssetParser::ExtractDependencies([[maybe_unused]] const GUID& referrer,
                                               [[maybe_unused]] const AssetMetadata& metadata,
                                               [[maybe_unused]] DepEdgeSink& sink) const
{
    return true;
}

} // namespace GameEngine
