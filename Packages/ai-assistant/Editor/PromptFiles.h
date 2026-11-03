#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace GameEngine
{

/// How a file picked for a prompt travels to the agent.
enum class PromptFileKind : uint8_t
{
    /// Sent as an image the model sees (PNG, JPEG, GIF or WebP; a large one scaled down).
    Image,
    /// Sent as text after the prompt, in a `<file>` block.
    Text,
};

/// A file the user picked with the prompt's + button, read when it was picked.
struct PromptFile
{
    /// Where it was read from; Codex receives images by this path.
    std::filesystem::path Path;
    /// The file name the chips and the `<file>` block show.
    std::string Name;
    PromptFileKind Kind = PromptFileKind::Text;
    /// The image's media type ("image/png"); empty for text.
    std::string MediaType;
    /// What is sent: the image's file, or its scaled-down copy when the file is over
    /// kMaxImageBytes; or the text (valid UTF-8, at most kMaxTextFileBytes).
    std::string Bytes;
    /// The text was longer than kMaxTextFileBytes and Bytes holds its start.
    bool Cut = false;

    bool operator==(const PromptFile&) const = default;
};

/// A picked file read for a prompt, or why it could not be.
struct PromptFileLoad
{
    std::optional<PromptFile> File;
    /// Why the file was not added, naming it and the fix; empty when File is set.
    std::string Refusal;
};

/// The files a prompt carries as context: images the model sees, and text files appended to the
/// prompt as `<file>` blocks, in the same way mark-ups are (MarkupContext). The block format is
/// documented in docs/Editor/ai-assistant.html.
class PromptFiles
{
public:
    /// The largest image sent: the Messages API's limit per image.
    static constexpr std::size_t kMaxImageBytes = 5 * 1024 * 1024;
    /// The longest edge a scaled-down image is sent at; the model scales a longer one down to it.
    static constexpr uint32_t kMaxImageLongEdge = 1568;
    /// The largest image file read: a larger one is refused before it is decoded.
    static constexpr std::size_t kMaxSourceImageBytes = 50 * 1024 * 1024;
    /// The most pixels an image to scale down may declare: a larger one is refused by its header,
    /// before it is decoded.
    static constexpr uint64_t kMaxSourceImagePixels = 64'000'000;
    /// The most of one text file a block holds; a longer file is cut and its block says so.
    static constexpr std::size_t kMaxTextFileBytes = 64 * 1024;
    /// The most a turn's file blocks hold; the files past it are named on one line, not shown.
    static constexpr std::size_t kMaxTurnTextBytes = 256 * 1024;
    /// The most files one prompt carries.
    static constexpr std::size_t kMaxFiles = 20;

    /// Reads `path` as an image (by its first bytes) or as UTF-8 text. An image file over
    /// kMaxImageBytes is scaled down to kMaxImageLongEdge and kept as a PNG, or as a JPEG when
    /// the PNG is still over kMaxImageBytes. Refuses, naming the file and the fix: an unreadable
    /// file, an empty one, an image file over kMaxSourceImageBytes, a large image declaring more than
    /// kMaxSourceImagePixels when it needs decoding, an image that cannot be decoded or still does
    /// not fit, and anything that is neither an image nor text.
    static PromptFileLoad Load(const std::filesystem::path& path);

    /// What the turn sends after the prompt for its text files: one `<file name="…">` block each,
    /// in order, within kMaxTurnTextBytes; empty when there is no text file. A `</file` in file
    /// content is escaped so only the generated closing tag ends the block.
    static std::string Blocks(std::span<const PromptFile> files);

    /// The media type of an image whose file starts with `head`; empty when it is none of the
    /// four the providers accept.
    static std::string_view ImageMediaType(std::string_view head);
};

/// `path` as UTF-8 text. On Windows path::string() converts through the ANSI code page and throws
/// for a name outside it.
inline std::string PathUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace GameEngine
