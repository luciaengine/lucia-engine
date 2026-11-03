#include "PromptFiles.h"

#include "Engine/Rendering/RgbaImageOps.h"
#include "Types/Utf8.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

namespace GameEngine
{
namespace
{
// A name or path inside a block's attribute: the quote and the markup characters escaped.
std::string EscapeAttribute(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c; break;
        }
    }
    return out;
}

// The quality of the JPEG a scaled-down picture falls back to when its PNG is still too large.
constexpr int kScaledJpegQuality = 90;

// `bytes` as megabytes with one decimal, "5.7 MB".
std::string Megabytes(std::size_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// The image the turn sends for an image file: the file itself when it is at most kMaxImageBytes
// (the model scales a large picture down itself), else the picture scaled down to
// kMaxImageLongEdge as a PNG, or as a JPEG when the PNG is still too large. Empty `MediaType`
// with the reason in `Refusal` when it cannot be sent.
struct SentImage
{
    std::string Bytes;
    std::string MediaType;
    std::string Refusal;
};

SentImage ImageToSend(const std::string& name, std::string bytes, std::string_view mediaType)
{
    if (bytes.size() <= PromptFiles::kMaxImageBytes)
        return {std::move(bytes), std::string(mediaType), {}};
    uint32_t width = 0, height = 0;
    if (ReadImageDimensions(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), width, height) &&
        uint64_t{width} * height > PromptFiles::kMaxSourceImagePixels)
        return {{}, {}, name + " is " + std::to_string(width) + " x " + std::to_string(height) +
                            " pixels; an image to scale down can have at most " +
                            std::to_string(PromptFiles::kMaxSourceImagePixels / 1'000'000) +
                            " megapixels, so save a smaller copy and add it"};
    const DecodedImage scaled = DownscaleRgbaToLongEdge(
        DecodeImageToRGBA(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), PromptFiles::kMaxImageLongEdge);
    if (!scaled.valid)
        return {{}, {}, name + " is " + Megabytes(bytes.size()) + " and could not be scaled down; save a copy under " +
                            Megabytes(PromptFiles::kMaxImageBytes) + " and add it"};
    if (std::vector<uint8_t> png = EncodeRgbaPng(scaled); !png.empty() && png.size() <= PromptFiles::kMaxImageBytes)
        return {std::string(png.begin(), png.end()), "image/png", {}};
    if (std::vector<uint8_t> jpeg = EncodeRgbaJpeg(scaled, kScaledJpegQuality);
        !jpeg.empty() && jpeg.size() <= PromptFiles::kMaxImageBytes)
        return {std::string(jpeg.begin(), jpeg.end()), "image/jpeg", {}};
    return {{}, {}, name + " could not be scaled down to " + Megabytes(PromptFiles::kMaxImageBytes) +
                        "; save a smaller copy and add it"};
}

// The file's text with every `</file` written `&lt;/file`, so the text cannot end its block.
std::string EscapeCloseTag(std::string_view text)
{
    constexpr std::string_view kCloseTag = "</file";
    std::string out;
    out.reserve(text.size());
    std::size_t start = 0;
    for (std::size_t at = text.find(kCloseTag); at != std::string_view::npos; at = text.find(kCloseTag, start))
    {
        out.append(text, start, at - start);
        out += "&lt;/file";
        start = at + kCloseTag.size();
    }
    out.append(text, start);
    return out;
}

std::string Block(const PromptFile& file)
{
    const std::u8string path = file.Path.generic_u8string();
    std::string block = "<file name=\"" + EscapeAttribute(file.Name) + "\" path=\"" +
                        EscapeAttribute(std::string(path.begin(), path.end())) + "\">\n" + EscapeCloseTag(file.Bytes);
    if (!block.empty() && block.back() != '\n')
        block += '\n';
    if (file.Cut)
        block += "[the file continues; only its first " + std::to_string(PromptFiles::kMaxTextFileBytes / 1024) +
                 " KB are shown]\n";
    block += "</file>";
    return block;
}
} // namespace

std::string_view PromptFiles::ImageMediaType(std::string_view head)
{
    if (head.starts_with("\x89PNG\r\n\x1A\n"))
        return "image/png";
    if (head.starts_with("\xFF\xD8\xFF"))
        return "image/jpeg";
    if (head.starts_with("GIF87a") || head.starts_with("GIF89a"))
        return "image/gif";
    if (head.size() >= 12 && head.starts_with("RIFF") && head.substr(8, 4) == "WEBP")
        return "image/webp";
    return {};
}

PromptFileLoad PromptFiles::Load(const std::filesystem::path& path)
{
    const std::string name = PathUtf8(path.filename());
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {std::nullopt, name + " could not be read: " + error.message()};
    if (size == 0)
        return {std::nullopt, name + " is empty"};

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {std::nullopt, name + " could not be opened"};

    // Enough to tell an image by its first bytes, and to read a text file up to its cap.
    std::string head(std::min<std::size_t>(size, 16), '\0');
    stream.read(head.data(), static_cast<std::streamsize>(head.size()));
    const std::string_view mediaType = ImageMediaType(head);

    PromptFile file;
    file.Path = path;
    file.Name = name;
    if (!mediaType.empty())
    {
        if (size > kMaxSourceImageBytes)
            return {std::nullopt, name + " is " + Megabytes(size) + "; an image file can be at most " +
                                      Megabytes(kMaxSourceImageBytes)};
        SentImage image = ImageToSend(name, head + std::string(std::istreambuf_iterator<char>(stream), {}), mediaType);
        if (image.MediaType.empty())
            return {std::nullopt, std::move(image.Refusal)};
        file.Kind = PromptFileKind::Image;
        file.MediaType = std::move(image.MediaType);
        file.Bytes = std::move(image.Bytes);
        return {std::move(file), {}};
    }

    // Text: read one character past the cap to know whether it was cut, then cut on a
    // character boundary.
    std::string rest(std::min<std::size_t>(size, kMaxTextFileBytes + 4) - head.size(), '\0');
    stream.read(rest.data(), static_cast<std::streamsize>(rest.size()));
    std::string text = head + rest.substr(0, static_cast<std::size_t>(stream.gcount()));
    file.Cut = size > kMaxTextFileBytes;
    if (file.Cut)
        text.resize(Utf8PrefixLength(text, kMaxTextFileBytes));
    if (text.find('\0') != std::string::npos || !IsValidUtf8(text))
        return {std::nullopt, name + " is not text or an image (PNG, JPEG, GIF or WebP), so it cannot be sent"};
    file.Kind = PromptFileKind::Text;
    file.Bytes = std::move(text);
    return {std::move(file), {}};
}

std::string PromptFiles::Blocks(std::span<const PromptFile> files)
{
    std::string blocks;
    std::string leftOut;
    for (const PromptFile& file : files)
    {
        if (file.Kind != PromptFileKind::Text)
            continue;
        std::string block = Block(file);
        if (!leftOut.empty() || blocks.size() + block.size() > kMaxTurnTextBytes)
        {
            leftOut += (leftOut.empty() ? "" : ", ") + file.Name;
            continue;
        }
        blocks += (blocks.empty() ? "" : "\n\n") + block;
    }
    if (!leftOut.empty())
        blocks += (blocks.empty() ? "" : "\n\n") + std::string("[files left out: the turn's files are over ") +
                  std::to_string(kMaxTurnTextBytes / 1024) + " KB: " + leftOut + "]";
    return blocks;
}

} // namespace GameEngine
