#pragma once

#include "UI/Controls/Label.h"
#include "UI/Controls/MarkdownText.h"
#include "UI/UIManager.h"

#include <array>

namespace GameEngine
{

// The owning MarkdownText owns the whole subtree. The binding is refreshed with
// its spans before layout or input can observe a replaced block.
class MarkdownTextRun final : public Label
{
public:
    void BindSelection(MarkdownText& owner, size_t spanIndex)
    {
        m_Owner = &owner;
        m_SpanIndex = spanIndex;
    }

    void OnPostLayout() override
    {
        if (!m_Owner || !m_Owner->HasTextSelection())
            return;
        const auto& style = GetResolvedStyle();
        const std::array<float, 22> geometry = {GetLayoutX(), GetLayoutY(), GetLayoutWidth(), GetLayoutHeight(),
            style.Visual.FontSize, style.Visual.LineHeight, style.Visual.LetterSpacing,
            style.Layout.Padding.Left, style.Layout.Padding.Top, style.Layout.Padding.Right, style.Layout.Padding.Bottom,
            style.Layout.BorderWidth.Left, style.Layout.BorderWidth.Top,
            style.Layout.BorderWidth.Right, style.Layout.BorderWidth.Bottom,
            static_cast<float>(style.Visual.FontWeight), static_cast<float>(style.Visual.FontStyle),
            static_cast<float>(style.Visual.WhiteSpace), static_cast<float>(style.Visual.WordBreak),
            static_cast<float>(style.Visual.OverflowWrap), static_cast<float>(style.Visual.TextAlign),
            GetOwnerManager()->GetContentScale()};
        if (geometry != m_Geometry || style.Visual.FontFamily != m_FontFamily)
        {
            m_Geometry = geometry;
            m_FontFamily = style.Visual.FontFamily;
            m_Owner->InvalidateSelectionGeometry();
            MarkDirty(VisualDirty);
        }
    }

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& context, const ResolvedStyle& style,
                              float x, float y, float, float) override
    {
        if (m_Owner && m_Owner->HasTextSelection())
            m_Owner->EmitSelectionFor(*this, m_SpanIndex, context, style, x, y);
    }

private:
    MarkdownText* m_Owner = nullptr;
    size_t m_SpanIndex = 0;
    std::array<float, 22> m_Geometry{};
    std::shared_ptr<const std::vector<std::string>> m_FontFamily;
};

} // namespace GameEngine
