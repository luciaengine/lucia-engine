#include "UI/Controls/MarkdownText.h"
#include "MarkdownTextRun.h"

#include "Input/KeyCodes.h"
#include "Input/InputSystem.h"
#include "../Parsers/MarkdownParser.h"
#include "Rendering/Text/TextLayout.h"
#include "UI/Controls/Label.h"
#include "UI/TextSelectionFill.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIPlatform.h"
#include "UI/Utf8Helpers.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace GameEngine
{

struct MarkdownText::SelectionState
{
    struct Span
    {
        UIElement* Element;
        size_t Start;
        std::string Text;
        size_t FirstLine = 0;
        size_t LineCount = 0;
    };
    struct Line
    {
        size_t SpanIndex;
        size_t Start;
        size_t End;
        float X;
        float Y;
        float Height;
        float Scale;
        Rendering::Text::FontAtlas::MeasureResult Measured;
    };
    std::vector<Span> Spans;
    std::vector<Line> Lines;
    std::string Text;
    size_t Anchor = 0;
    size_t Caret = 0;
    bool Selecting = false;
    bool Dragged = false;
    bool GeometryDirty = true;
    float PressX = 0;
    float PressY = 0;

    void Collect(UIElement& element, MarkdownText& owner)
    {
        if (element.HasClass("markdown-word") || element.HasClass("markdown-code-text") ||
            element.HasClass("markdown-list-marker"))
        {
            const std::string& text = element.GetTextContent();
            if (!text.empty())
            {
                if (auto* run = dynamic_cast<MarkdownTextRun*>(&element))
                    run->BindSelection(owner, Spans.size());
                Spans.push_back({&element, Text.size(), text});
                Text += text;
            }
            return;
        }
        for (const auto& child : element.GetChildren())
            if (child)
                Collect(*child, owner);
    }

    void Geometry(UIManager& manager)
    {
        if (!GeometryDirty)
            return;
        Lines.clear();
        const float scale = std::max(0.01f, manager.GetContentScale());
        using namespace Rendering::Text;
        for (size_t index = 0; index < Spans.size(); ++index)
        {
            Span& span = Spans[index];
            span.FirstLine = Lines.size();
            span.LineCount = 0;
            const auto& style = span.Element->GetResolvedStyle();
            auto* font = manager.ResolveFontForStyle(style);
            if (!font)
                continue;
            const auto& box = style.Layout;
            const auto& visual = style.Visual;
            const float left = box.Padding.Left + box.BorderWidth.Left;
            const float top = box.Padding.Top + box.BorderWidth.Top;
            const float width = std::max(0.0f, span.Element->GetLayoutWidth() - left -
                                        box.Padding.Right - box.BorderWidth.Right);
            const float height = span.Element->GetLayoutHeight() - top - box.Padding.Bottom - box.BorderWidth.Bottom;
            StyledRun run;
            run.Font = font;
            run.Text = span.Text;
            run.PixelSize = std::max(1.0f, visual.FontSize * scale);
            run.LetterSpacingPx = visual.LetterSpacing * scale;
            const float lineBox = TextLayout::ResolveLineBoxPx(visual.LineHeight, visual.FontSize, scale);
            const float advance = lineBox > 0 ? lineBox : font->GetFontLineMetrics(run.PixelSize).height;
            const bool nowrap = visual.WhiteSpace == WhiteSpace::Pre || visual.WhiteSpace == WhiteSpace::NoWrap;
            const auto policy = ResolveTextBreakPolicy(visual.WordBreak, visual.OverflowWrap);
            const auto wordBreak = policy == WordBreak::BreakAll ? TextLayout::WordBreak::BreakAll :
                                   policy == WordBreak::KeepAll ? TextLayout::WordBreak::KeepAll :
                                   policy == WordBreak::BreakWord ? TextLayout::WordBreak::BreakWord :
                                   TextLayout::WordBreak::Normal;
            const auto shaped = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1),
                                                           nowrap ? 0.0f : width * scale, lineBox, wordBreak);
            const float offsetY = shaped.LineBreaks.size() <= 1 ? (height - advance / scale) * 0.5f : 0.0f;
            const float firstBaseline = shaped.LineBreaks.empty() ? 0.0f : shaped.LineBreaks.front().BaselineY;
            for (const auto& line : shaped.LineBreaks)
            {
                const auto text = std::string_view(span.Text).substr(line.ByteStart, line.ByteEnd - line.ByteStart);
                const float alignment = visual.TextAlign == TextAlign::Center ? (width - line.Width / scale) * 0.5f :
                                        visual.TextAlign == TextAlign::Right ? width - line.Width / scale : 0.0f;
                ++span.LineCount;
                Lines.push_back({index, span.Start + line.ByteStart, span.Start + line.ByteEnd,
                                 span.Element->GetLayoutX() + left + alignment,
                                 span.Element->GetLayoutY() + top + offsetY + (line.BaselineY - firstBaseline) / scale,
                                 advance / scale, scale, font->MeasureText(text, run.PixelSize, run.LetterSpacingPx)});
            }
        }
        GeometryDirty = false;
    }

    const Line* Hit(float x, float y) const
    {
        const Line* best = nullptr;
        float distance = std::numeric_limits<float>::max();
        for (const Line& line : Lines)
        {
            const float dx = std::max({line.X - x, x - line.X - line.Measured.metrics.width / line.Scale, 0.0f});
            const float dy = std::max({line.Y - y, y - line.Y - line.Height, 0.0f});
            const float candidate = dx * dx + dy * dy;
            if (candidate < distance)
            {
                distance = candidate;
                best = &line;
            }
        }
        return best;
    }

    size_t Position(const Line& line, float x) const
    {
        const auto& positions = line.Measured.caretXByByte;
        if (positions.empty())
            return line.Start;
        const auto text = std::string_view(Spans[line.SpanIndex].Text).substr(
            line.Start - Spans[line.SpanIndex].Start, line.End - line.Start);
        const float target = (x - line.X) * line.Scale;
        size_t best = 0;
        for (size_t offset = 0; offset < text.size();)
        {
            offset = Utf8::Next(text, offset);
            if (offset < positions.size() && std::abs(positions[offset] - target) < std::abs(positions[best] - target))
                best = offset;
        }
        return line.Start + best;
    }
};

MarkdownText::MarkdownText()
{
    AddClass("markdown-text");
    SetFocusable(true);
}

MarkdownText::~MarkdownText() = default;

void MarkdownText::RefreshSelection()
{
    if (!m_Selection)
        m_Selection = std::make_unique<SelectionState>();
    auto& selection = *m_Selection;
    const std::string previous = std::move(selection.Text);
    selection.Text.clear();
    selection.Spans.clear();
    for (UIElement* block : m_BlockElements)
    {
        if (!selection.Text.empty())
            selection.Text += "\n\n";
        selection.Collect(*block, *this);
    }
    const size_t end = std::max(selection.Anchor, selection.Caret);
    if (end > selection.Text.size() || selection.Text.substr(0, end) != previous.substr(0, end))
    {
        selection.Anchor = selection.Caret = 0;
        selection.Selecting = false;
        MarkDirtySubtree(VisualDirty);
    }
    selection.GeometryDirty = true;
    MarkDirty(VisualDirty);
}

bool MarkdownText::HasTextSelection() const
{
    return m_Selection && m_Selection->Anchor != m_Selection->Caret;
}

void MarkdownText::InvalidateSelectionGeometry()
{
    if (m_Selection)
        m_Selection->GeometryDirty = true;
}

void MarkdownText::OnEvent(UIEvent& event)
{
    if (!m_Selection || !GetOwnerManager())
        return;
    auto& selection = *m_Selection;
    if (event.Id == kEventFocusIn || event.Id == kEventFocusOut)
    {
        MarkDirtySubtree(VisualDirty);
        return;
    }
    if (event.Id == kEventKeyDown)
    {
        if (Input::IsPrimaryShortcutModifier(event.Mods) && event.Key == Input::kKeyCode_A)
        {
            selection.Anchor = 0;
            selection.Caret = selection.Text.size();
        }
        else if (Input::IsPrimaryShortcutModifier(event.Mods) &&
                 (event.Key == Input::kKeyCode_C || event.Key == Input::kKeyCode_X))
        {
            if (selection.Anchor != selection.Caret && event.Platform)
            {
                const size_t begin = std::min(selection.Anchor, selection.Caret);
                const size_t end = std::max(selection.Anchor, selection.Caret);
                event.Platform->SetClipboardText(selection.Text.substr(begin, end - begin).c_str());
            }
        }
        else if ((Input::IsPrimaryShortcutModifier(event.Mods) && event.Key == Input::kKeyCode_V) ||
                 event.Key == Input::kKeyCode_Backspace || event.Key == Input::kKeyCode_Delete)
        {
            // A read-only document consumes edit chords instead of letting the application's
            // scene paste/delete shortcuts act on an unrelated selection.
        }
        else if (event.Key == Input::kKeyCode_Escape && selection.Anchor != selection.Caret)
            selection.Anchor = selection.Caret;
        else
            return;
        event.Stop();
        MarkDirtySubtree(VisualDirty);
        return;
    }
    if (event.Id == kEventMouseDown && event.Button == 0)
    {
        // Layout can move while selection is empty and the run geometry cache is idle.
        selection.GeometryDirty = true;
        selection.Geometry(*GetOwnerManager());
        const auto* line = selection.Hit(event.X, event.Y);
        if (!line)
            return;
        GetOwnerManager()->FocusElement(this);
        selection.Caret = selection.Position(*line, event.X);
        if ((event.Mods & Input::kModShift) == 0)
            selection.Anchor = selection.Caret;
        selection.PressX = event.X;
        selection.PressY = event.Y;
        selection.Selecting = true;
        selection.Dragged = false;
        event.Capture(this);
    }
    else if (event.Id == kEventMouseMove && selection.Selecting)
    {
        selection.Geometry(*GetOwnerManager());
        if (const auto* line = selection.Hit(event.X, event.Y))
            selection.Caret = selection.Position(*line, event.X);
        selection.Dragged |= std::hypot(event.X - selection.PressX, event.Y - selection.PressY) > 3.0f;
    }
    else if (event.Id == kEventMouseUp && selection.Selecting && event.Button == 0)
    {
        selection.Selecting = false;
        // Capture redirects the release to this control. A click still reaches the original
        // word's release handlers (links); a drag only completes the selection.
        if (!selection.Dragged && selection.Anchor == selection.Caret)
        {
            selection.Geometry(*GetOwnerManager());
            if (const auto* line = selection.Hit(event.X, event.Y))
            {
                UIEvent click = event;
                click.Handled = false;
                click.Target = selection.Spans[line->SpanIndex].Element;
                click.CurrentTarget = click.Target;
                event.Stop();
                MarkDirtySubtree(VisualDirty);
                click.Target->DispatchEvent(click);
                return;
            }
        }
    }
    else if (event.Id == kEventMouseCancel)
        selection.Selecting = false;
    else
        return;
    event.Stop();
    MarkDirtySubtree(VisualDirty);
}

void MarkdownText::EmitSelectionFor(UIElement& element, size_t spanIndex, UI::PrimitiveEmitContext& context,
                                    const ResolvedStyle& style, float x, float y)
{
    if (!m_Selection || m_Selection->Anchor == m_Selection->Caret || !GetOwnerManager())
        return;
    if (context.OffThread)
    {
        if (context.EscalateFlag)
            *context.EscalateFlag = true;
        return;
    }
    auto& selection = *m_Selection;
    selection.Geometry(*GetOwnerManager());
    const size_t begin = std::min(selection.Anchor, selection.Caret);
    const size_t end = std::max(selection.Anchor, selection.Caret);
    const uint32_t color = UI::PackedTextSelectionFill(style.Visual, IsPseudoFocused());
    const float scale = context.ContentScale;
    if (spanIndex >= selection.Spans.size() || selection.Spans[spanIndex].Element != &element)
        return;
    const auto& span = selection.Spans[spanIndex];
    for (size_t index = span.FirstLine; index < span.FirstLine + span.LineCount; ++index)
    {
        const auto& line = selection.Lines[index];
        if (end <= line.Start || begin >= line.End)
            continue;
        const auto& positions = line.Measured.caretXByByte;
        if (positions.empty())
            continue;
        const float first = positions[std::min(std::max(begin, line.Start) - line.Start, positions.size() - 1)];
        const float last = positions[std::min(std::min(end, line.End) - line.Start, positions.size() - 1)];
        context.Emit(UI::MakeRect(x + (line.X - element.GetLayoutX()) * scale + std::min(first, last),
                                  y + (line.Y - element.GetLayoutY()) * scale,
                                  std::abs(last - first), line.Height * scale, color));
    }
}

} // namespace GameEngine
