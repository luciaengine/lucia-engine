#include "CssInspector/CssInspector.h"

#include "Mathematics/Vector2.h"
#include "UI/UIManager.h"
#include "UI/UIElement.h"
#include "UI/Controls/Label.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleOverrides.h"
#include "UI/UIStyle.h"
#include "UI/StyleProperties.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

namespace
{
    constexpr const char* kStyleAssetPath = "UI/controls/CssInspector/CssInspector.css";

    enum class SegmentRole : uint8_t
    {
        Label, // property names, punctuation, headings
        Value, // selectors and property values
    };
    struct Segment { std::string Text; SegmentRole Role; };


    static std::string FormatLength(const StyleLength& L) {
        if (L.IsAuto()) return "auto";
        if (L.IsPercent()) return std::to_string(static_cast<int>(L.Value)) + "%";
        return std::to_string(static_cast<int>(L.Value)) + "px";
    }
    static std::string FormatColorArgb(uint32_t c) { char b[10]; std::snprintf(b, sizeof(b), "#%02X%02X%02X%02X", (c>>24)&0xFF, (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF); return b; }
    static std::string FormatColorRgb(uint32_t c) { char b[8]; std::snprintf(b, sizeof(b), "%06X", c & 0xFFFFFFu); return std::string(b); }

    // Parse color code from tooltip text; returns 0xAARRGGBB for swatch background, or nullopt if not a color.
    static std::optional<uint32_t> ParseColorFromText(std::string_view s)
    {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        auto readHex = [&hex](std::string_view t, size_t i, int digits) -> std::optional<uint32_t> {
            uint32_t v = 0;
            for (int d = 0; d < digits && i + d < t.size(); ++d) {
                int h = hex(t[i + d]);
                if (h < 0) return std::nullopt;
                v = (v << 4) | static_cast<uint32_t>(h);
            }
            return (digits == 6) ? (0xFF000000u | v) : v; // 6-digit -> opaque
        };
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        if (s.size() >= 1 && s[0] == '#')
        {
            if (s.size() >= 9) { auto v = readHex(s, 1, 8); if (v) return *v; }
            if (s.size() >= 7) { auto v = readHex(s, 1, 6); if (v) return *v; }
            return std::nullopt;
        }
        if (s.size() >= 6)
        {
            auto v = readHex(s, 0, 6);
            if (v) return *v;
        }
        return std::nullopt;
    }

    static void AppendSegment(std::vector<Segment>& segs, const std::string& text, SegmentRole role)
    {
        if (!text.empty()) segs.push_back(Segment{text, role});
    }

    static const char* StylePropertyIdName(StylePropertyId id)
    {
        switch (id)
        {
        case StylePropertyId::Display: return "display";
        case StylePropertyId::FlexDir: return "flex-direction";
        case StylePropertyId::Position: return "position";
        case StylePropertyId::PositionLeft: return "left";
        case StylePropertyId::PositionTop: return "top";
        case StylePropertyId::PositionRight: return "right";
        case StylePropertyId::PositionBottom: return "bottom";
        case StylePropertyId::ZIndex: return "z-index";
        case StylePropertyId::Width: return "width";
        case StylePropertyId::Height: return "height";
        case StylePropertyId::MinWidth: return "min-width";
        case StylePropertyId::MinHeight: return "min-height";
        case StylePropertyId::MaxWidth: return "max-width";
        case StylePropertyId::MaxHeight: return "max-height";
        case StylePropertyId::FlexGrow: return "flex-grow";
        case StylePropertyId::FlexShrink: return "flex-shrink";
        case StylePropertyId::Gap: return "gap";
        case StylePropertyId::Opacity: return "opacity";
        case StylePropertyId::Visibility: return "visibility";
        case StylePropertyId::Color: return "color";
        case StylePropertyId::BackgroundColor: return "background-color";
        case StylePropertyId::FontSize: return "font-size";
        case StylePropertyId::FontWeight: return "font-weight";
        case StylePropertyId::FontFamily: return "font-family";
        case StylePropertyId::Overflow: return "overflow";
        case StylePropertyId::TextOverflow: return "text-overflow";
        case StylePropertyId::PaddingTop: return "padding-top";
        case StylePropertyId::PaddingRight: return "padding-right";
        case StylePropertyId::PaddingBottom: return "padding-bottom";
        case StylePropertyId::PaddingLeft: return "padding-left";
        case StylePropertyId::MarginTop: return "margin-top";
        case StylePropertyId::MarginRight: return "margin-right";
        case StylePropertyId::MarginBottom: return "margin-bottom";
        case StylePropertyId::MarginLeft: return "margin-left";
        default: return nullptr;
        }
    }

    static void AppendStyleOverrideSegments(std::vector<Segment>& segs, const StyleOverrides& ovr)
    {
        ovr.ForEachProp([&](StylePropertyId id, const StyleOverrideEntry& entry)
        {
            const char* name = StylePropertyIdName(id);
            if (!name) return;
            std::string label = std::string("  ") + name + ": ";
            std::string value;
            if (entry.keyword != StyleKeyword::None)
            {
                value = (entry.keyword == StyleKeyword::Inherit) ? "inherit" :
                        (entry.keyword == StyleKeyword::Initial) ? "initial" : "unset";
            }
            else if (auto* f = std::get_if<float>(&entry.value))
                value = std::to_string(*f);
            else if (auto* i = std::get_if<int>(&entry.value))
                value = std::to_string(*i);
            else if (auto* u = std::get_if<uint32_t>(&entry.value))
            {
                char buf[12];
                std::snprintf(buf, sizeof(buf), "#%08X", *u);
                value = buf;
            }
            else if (auto* b = std::get_if<bool>(&entry.value))
                value = *b ? "true" : "false";
            else if (auto* len = std::get_if<StyleLength>(&entry.value))
                value = FormatLength(*len);
            else
                value = "(...)";

            AppendSegment(segs, label, SegmentRole::Label);
            AppendSegment(segs, value + ";\n", SegmentRole::Value);
        });
    }

    static void FormatCssSummarySegments(const UIManager* ui, const UIElement* el, std::vector<Segment>& segs)
    {
        if (!ui || !el) return;
        AppendSegment(segs, ui->GetElementSelector(el), SegmentRole::Value);
        const ResolvedStyle* rs = ui->TryGetResolvedStyleFor(el);
        if (rs)
        {
            const auto& lo = rs->Layout;
            const auto& vs = rs->Visual;
            auto px = [](float v) { return std::to_string(static_cast<int>(v)); };
            auto len = [](const StyleLength& L) {
                if (L.IsAuto()) return std::string("auto");
                if (L.IsPercent()) return std::to_string(static_cast<int>(L.Value)) + "%";
                return std::to_string(static_cast<int>(L.Value)) + "px";
            };
            auto prop = [&segs](const char* label, const std::string& value) {
                AppendSegment(segs, std::string(label), SegmentRole::Label);
                AppendSegment(segs, value + ";\n", SegmentRole::Value);
            };
            AppendSegment(segs, " {\n", SegmentRole::Label);
            prop("  display: ", std::string(lo.DisplayMode == DisplayMode::Flex ? "flex" : lo.DisplayMode == DisplayMode::Block ? "block" : lo.DisplayMode == DisplayMode::Inline ? "inline" : "none"));
            prop("  position: ", std::string(lo.PositionType == PositionType::Absolute ? "absolute" : "relative"));
            if (!lo.Width.IsAuto()) prop("  width: ", len(lo.Width));
            if (!lo.Height.IsAuto()) prop("  height: ", len(lo.Height));
            if (lo.Padding.Top != 0.f || lo.Padding.Left != 0.f) prop("  padding: ", px(lo.Padding.Top) + " " + px(lo.Padding.Right) + " " + px(lo.Padding.Bottom) + " " + px(lo.Padding.Left));
            if (lo.Margin.Top != 0.f || lo.Margin.Left != 0.f) prop("  margin: ", px(lo.Margin.Top) + " " + px(lo.Margin.Right) + " " + px(lo.Margin.Bottom) + " " + px(lo.Margin.Left));
            if (vs.HasColor) { AppendSegment(segs, "  color: #", SegmentRole::Label); AppendSegment(segs, FormatColorRgb(vs.Color) + ";\n", SegmentRole::Value); }
            if ((vs.BackgroundColor & 0xFF000000u) != 0) { AppendSegment(segs, "  background: ", SegmentRole::Label); AppendSegment(segs, FormatColorArgb(vs.BackgroundColor) + ";\n", SegmentRole::Value); }
            if (vs.HasFontSize) prop("  font-size: ", px(vs.FontSize) + "px");
            if (vs.HasFontFamily && vs.FontFamily && !vs.FontFamily->empty())
            {
                std::string fam;
                for (size_t i = 0; i < vs.FontFamily->size(); ++i)
                {
                    if (i) fam += ", ";
                    const std::string& s = (*vs.FontFamily)[i];
                    if (s.find(' ') != std::string::npos || s.find(',') != std::string::npos)
                        fam += "\"" + s + "\"";
                    else
                        fam += s;
                }
                prop("  font-family: ", fam);
            }
            if (vs.HasFontWeight && vs.FontWeight != 400) prop("  font-weight: ", std::to_string(vs.FontWeight));
            if (vs.HasFontStyle && vs.FontStyle != FontStyle::Normal)
                prop("  font-style: ", std::string(vs.FontStyle == FontStyle::Italic ? "italic" : vs.FontStyle == FontStyle::Oblique ? "oblique" : "normal"));
            if (lo.FlexGrow != 0.f) prop("  flex-grow: ", std::to_string(lo.FlexGrow));
            if (lo.Gap != 0.f) prop("  gap: ", px(lo.Gap) + (lo.GapIsPercent ? "%" : "px"));
            AppendSegment(segs, "}", SegmentRole::Label);
        }
        else
            AppendSegment(segs, " { (no resolved style) }", SegmentRole::Label);

        const auto& inlineOvr = el->InlineOverrides();
        if (!inlineOvr.IsEmpty())
        {
            AppendSegment(segs, "\n/* inline style */\n", SegmentRole::Label);
            AppendStyleOverrideSegments(segs, inlineOvr);
        }
    }

    static std::string FormatCssSummary(const UIManager* ui, const UIElement* el)
    {
        std::vector<Segment> segs;
        FormatCssSummarySegments(ui, el, segs);
        std::string out;
        for (const auto& s : segs) out += s.Text;
        return out;
    }

    // Full selector path from root down to el (for display and copy).
    static std::string BuildPathString(const UIManager* ui, const UIElement* el)
    {
        if (!ui || !el) return std::string();
        std::vector<UIElement*> stack;
        for (UIElement* e = const_cast<UIElement*>(el); e; e = e->GetParent())
            stack.push_back(e);
        std::string path;
        for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i)
        {
            if (i < static_cast<int>(stack.size()) - 1) path += " > ";
            path += ui->GetElementSelector(stack[static_cast<size_t>(i)]);
        }
        return path;
    }
}

UIElement* CssInspector::GetDisplayedElement(UIManager* ui) const
{
    if (!ui) return nullptr;
    UIElement* hovered = ui->GetHoveredElement();
    if (!hovered) return nullptr;
    std::vector<UIElement*> layers;
    for (UIElement* e = hovered; e; e = e->GetParent())
        layers.push_back(e);
    int idx = m_DisplayLayerIndex;
    if (idx >= static_cast<int>(layers.size())) idx = layers.size() > 0 ? static_cast<int>(layers.size()) - 1 : 0;
    if (idx < 0) idx = 0;
    return layers[static_cast<size_t>(idx)];
}

std::string CssInspector::GetSummaryForElement(const UIManager* ui, const UIElement* el) const
{
    return FormatCssSummary(ui, el);
}

std::string CssInspector::GetSummaryIncludingPathForElement(const UIManager* ui, const UIElement* el) const
{
    std::string path = BuildPathString(ui, el);
    std::string summary = FormatCssSummary(ui, el);
    if (path.empty()) return summary;
    return "path: " + path + "\n\n" + summary;
}

void CssInspector::OnScrollWheel(float deltaY)
{
    if (deltaY > 0.f)
        m_DisplayLayerIndex++;
    else if (deltaY < 0.f)
        m_DisplayLayerIndex--;
    if (m_DisplayLayerIndex < 0)
        m_DisplayLayerIndex = 0;
    // upper bound clamped in Update() when we have layer count
}

void CssInspector::Update(UIManager* ui, int windowWidth, int windowHeight)
{
    if (!ui)
        return;
    UIElement* root = ui->GetRootElement();
    if (!root)
        return;

    UIElement* overlay = root->FindById("css-inspector-overlay");
    UIElement* highlight = root->FindById("css-inspector-highlight");
    if (!m_Enabled)
    {
        if (overlay)
            overlay->Overrides().Set(Style::Display, DisplayMode::None);
        if (highlight)
            highlight->Overrides().Set(Style::Display, DisplayMode::None);
        return;
    }

    UIElement* hovered = ui->GetHoveredElement();
    if (!hovered)
    {
        if (overlay)
            overlay->Overrides().Set(Style::Display, DisplayMode::None);
        if (highlight)
            highlight->Overrides().Set(Style::Display, DisplayMode::None);
        return;
    }

    // Build layer stack: hovered (topmost) then parent, grandparent, ... up to root
    std::vector<UIElement*> layers;
    for (UIElement* e = hovered; e; e = e->GetParent())
        layers.push_back(e);

    // Reset display layer when hovered element changes; clamp to valid range
    const uint64_t hoveredId = hovered->GetInstanceId();
    if (hoveredId != m_LastHoveredInstanceId)
    {
        m_LastHoveredInstanceId = hoveredId;
        m_DisplayLayerIndex = 0;
    }
    const int numLayers = static_cast<int>(layers.size());
    if (m_DisplayLayerIndex >= numLayers)
        m_DisplayLayerIndex = numLayers > 0 ? numLayers - 1 : 0;
    if (m_DisplayLayerIndex < 0)
        m_DisplayLayerIndex = 0;

    UIElement* displayedEl = layers[static_cast<size_t>(m_DisplayLayerIndex)];

    // Yellow border around displayed layer. Layout rects are already in root (absolute) space.
    // Layout is content box: expand by padding and border so the highlight matches the drawn edge.
    float hx = displayedEl->GetLayoutX();
    float hy = displayedEl->GetLayoutY();
    float hw = displayedEl->GetLayoutWidth();
    float hh = displayedEl->GetLayoutHeight();
    const ResolvedStyle* hoverStyle = ui->TryGetResolvedStyleFor(displayedEl);
    if (hoverStyle)
    {
        const auto& pad = hoverStyle->Layout.Padding;
        const auto& bw = hoverStyle->Layout.BorderWidth;
        hx -= pad.Left + bw.Left;
        hy -= pad.Top + bw.Top;
        hw += pad.Left + pad.Right + bw.Left + bw.Right;
        hh += pad.Top + pad.Bottom + bw.Top + bw.Bottom;
        if (hw < 0.f) { hx += hw; hw = 0.f; }
        if (hh < 0.f) { hy += hh; hh = 0.f; }
    }
    if (!highlight)
    {
        auto hl = std::make_unique<UIElement>();
        hl->SetId("css-inspector-highlight");
        hl->AddClass("css-inspector-highlight");
        hl->RequestSubtreeStyleAssetPath(kStyleAssetPath);
        // Never hit-testable, even before the sheet resolves: an outline that took
        // the hover would inspect itself.
        hl->Overrides().Set(Style::PointerEvents, false);
        highlight = hl.get();
        root->AddChild(std::move(hl));
    }
    highlight->Overrides()
        .Set(Style::Display, DisplayMode::Block)
        .Set(Style::PositionLeft, StyleLength::Px(hx))
        .Set(Style::PositionTop, StyleLength::Px(hy))
        .Set(Style::Width, StyleLength::Px(hw))
        .Set(Style::Height, StyleLength::Px(hh));

    // Tooltip follows mouse (like tooltips)
    const Mathematics::Vector2 mousePosition = ui->GetMousePosition();
    const float tooltipOffset = 14.f;
    const float tooltipWidth = 475.f;
    const float tooltipHeight = 500.f;
    float tooltipX = mousePosition.x + tooltipOffset;
    float tooltipY = mousePosition.y + tooltipOffset;
    if (tooltipX + tooltipWidth > static_cast<float>(windowWidth))
        tooltipX = static_cast<float>(windowWidth) - tooltipWidth - 8.f;
    if (tooltipY + tooltipHeight > static_cast<float>(windowHeight))
        tooltipY = static_cast<float>(windowHeight) - tooltipHeight - 8.f;
    if (tooltipX < 8.f) tooltipX = 8.f;
    if (tooltipY < 8.f) tooltipY = 8.f;

    if (!overlay)
    {
        auto container = std::make_unique<UIElement>();
        container->SetId("css-inspector-overlay");
        container->AddClass("css-inspector-card");
        container->RequestSubtreeStyleAssetPath(kStyleAssetPath);
        container->Overrides().Set(Style::PointerEvents, false);
        overlay = container.get();
        root->AddChild(std::move(container));
    }

    overlay->Overrides()
        .Set(Style::Display, DisplayMode::Block)
        .Set(Style::PositionLeft, StyleLength::Px(tooltipX))
        .Set(Style::PositionTop, StyleLength::Px(tooltipY))
        .Set(Style::Width, StyleLength::Px(tooltipWidth))
        .Set(Style::Height, StyleLength::Px(tooltipHeight));

    // Build tooltip: layer line, then CSS block, then full path at bottom (wrapped: one selector per line)
    std::vector<Segment> segs;
    AppendSegment(segs, "layer ", SegmentRole::Label);
    AppendSegment(segs, std::to_string(m_DisplayLayerIndex + 1) + " of " + std::to_string(numLayers), SegmentRole::Value);
    AppendSegment(segs, " (scroll to change)\n", SegmentRole::Label);
    FormatCssSummarySegments(ui, displayedEl, segs);
    // Full path at bottom, wrapped (each selector on its own line)
    std::string pathStr = BuildPathString(ui, displayedEl);
    if (!pathStr.empty())
    {
        AppendSegment(segs, "\n\npath:\n", SegmentRole::Label);
        const char* sep = " > ";
        size_t pos = 0;
        bool first = true;
        while (pos <= pathStr.size())
        {
            size_t next = pathStr.find(sep, pos);
            if (next == std::string::npos) next = pathStr.size();
            std::string part(pathStr.substr(pos, next - pos));
            if (!part.empty())
            {
                AppendSegment(segs, first ? "  " : "  > ", SegmentRole::Label);
                AppendSegment(segs, part + "\n", SegmentRole::Value);
                first = false;
            }
            if (next >= pathStr.size()) break;
            pos = next + strlen(sep);
        }
    }
    overlay->RemoveAllChildren();

    using RowContent = std::vector<std::pair<std::string, SegmentRole>>;
    std::vector<RowContent> rows;
    RowContent currentRow;
    for (const Segment& s : segs)
    {
        size_t pos = 0;
        for (;;)
        {
            size_t next = s.Text.find('\n', pos);
            if (next == std::string::npos)
            {
                if (pos < s.Text.size())
                    currentRow.push_back({s.Text.substr(pos), s.Role});
                break;
            }
            if (pos < next)
                currentRow.push_back({s.Text.substr(pos, next - pos), s.Role});
            if (!currentRow.empty())
            {
                rows.push_back(std::move(currentRow));
                currentRow = RowContent();
            }
            pos = next + 1;
        }
    }
    if (!currentRow.empty())
        rows.push_back(std::move(currentRow));

    auto column = std::make_unique<UIElement>();
    column->AddClass("css-inspector-lines");
    for (const RowContent& rowContent : rows)
    {
        auto rowEl = std::make_unique<UIElement>();
        rowEl->AddClass("css-inspector-line");
        for (const auto& [text, role] : rowContent)
        {
            if (text.empty()) continue;
            std::optional<uint32_t> swatchColor = ParseColorFromText(text);
            if (swatchColor)
            {
                auto swatch = std::make_unique<UIElement>();
                swatch->AddClass("css-inspector-swatch");
                swatch->Overrides().Set(Style::BackgroundColor, *swatchColor);
                rowEl->AddChild(std::move(swatch));
            }
            auto label = std::make_unique<Label>();
            label->SetText(text);
            label->AddClass(role == SegmentRole::Label ? "css-inspector-label" : "css-inspector-value");
            rowEl->AddChild(std::move(label));
        }
        if (rowEl->GetChildren().size() > 0)
            column->AddChild(std::move(rowEl));
    }
    if (column->GetChildren().size() > 0)
        overlay->AddChild(std::move(column));
}

} // namespace GameEngine
