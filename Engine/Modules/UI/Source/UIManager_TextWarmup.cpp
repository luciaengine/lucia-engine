#include "UI/UIManager.h"
#include "UIManager/UIManager_Internal.h"

#include "Rendering/Text/FontAtlas.h"
#include "UI/UITextureRegistry.h"

#include <algorithm>
#include <vector>

namespace GameEngine
{
UIManager::TextWarmupStatus UIManager::PrepareTextSubtree(UIElement* root)
{
    if (!root || root->GetOwnerManager() != this) return TextWarmupStatus::Failed;
    if (!m_SdfTextureRegistry || m_FontResolvedDirty)
        return TextWarmupStatus::Pending;
    struct WarmupNode
    {
        UIElement* Element;
        std::vector<StylesheetHandle> Sheets;
        bool HiddenAncestor;
    };
    std::vector<UIElement*> ancestors;
    for (auto* parent = root->GetDfsParent(); parent; parent = parent->GetDfsParent())
        ancestors.push_back(parent);
    auto inheritedSheets = m_GlobalStylesheets;
    bool hiddenAncestor = false;
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
    {
        if ((*it)->IsDirty(UIElement::StyleDirty)) return TextWarmupStatus::Pending;
        MergeSheetsHandles(inheritedSheets, (*it)->GetStylesheets());
        hiddenAncestor |= (*it)->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None;
    }
    std::vector<WarmupNode> pending{{root, std::move(inheritedSheets), hiddenAncestor}};
    bool waiting = false;
    while (!pending.empty())
    {
        auto node = std::move(pending.back());
        pending.pop_back();
        auto* element = node.Element;
        MergeSheetsHandles(node.Sheets, element->GetStylesheets());
        // A clean root does not imply that descendant classes or inherited
        // font styles have been resolved. Never certify an old descendant face.
        if (element->IsDirty(UIElement::StyleDirty) || element->Overrides().NeedsCascadeRerun())
        {
            if (!node.HiddenAncestor) return TextWarmupStatus::Pending;
            std::vector<const Stylesheet*> sheets;
            std::vector<const StylesheetRuleIndex*> indices;
            for (const auto& sheet : node.Sheets)
            {
                sheets.push_back(sheet.get());
                indices.push_back(GetOrBuildRuleIndex(sheet.get()));
            }
            auto* parent = element->GetDfsParent();
            const auto* parentStyle = parent ? &parent->GetResolvedStyle() : nullptr;
            // Hidden descendants may never have entered the layout cascade.
            element->m_RuleCacheValid = false;
            TransitionRegistration transition = TransitionRegistration::None;
            ResolveCascadeForElement(element, sheets, indices, parentStyle, nullptr, transition);
            ApplyTransitionRegistration(element, transition);
            FinalizeElementStyle(element);
            ResolveFontSize(element->GetMutableResolvedStyle().Visual,
                parentStyle ? parentStyle->Visual.FontSize : kInitialFontSizePx);
            element->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
        const bool hidden = node.HiddenAncestor ||
            element->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None;
        if (!element->GetTextContent().empty())
        {
            const auto& style = element->GetResolvedStyle();
            auto* font = ResolveFontForStyle(style);
            if (!font) return TextWarmupStatus::Failed;
            // Resolution can request a preferred family and temporarily return
            // the default face. Do not certify that transient face as ready.
            if (!m_FontFamilyInFlight.empty()) waiting = true;
            Rendering::Text::FontAtlas::ShapeResult shaped;
            font->ShapeText(element->GetTextContent(),
                std::max(1.0f, style.Visual.FontSize * m_ContentScale), shaped,
                0xffffffff, style.Visual.LetterSpacing * m_ContentScale);
            const auto& pages = m_SdfTextureRegistry->RegisterSlugTextures(*font);
            for (const auto& glyph : shaped.glyphs)
            {
                if (glyph.isColor)
                {
                    if (m_SdfTextureRegistry->RegisterColorAtlasPage(*font, glyph.atlasPageIndex) == 0)
                        return TextWarmupStatus::Failed;
                }
                else if (glyph.pageIndex >= pages.size() || pages[glyph.pageIndex].CurveTexIdx == 0 ||
                         pages[glyph.pageIndex].BandTexIdx == 0)
                    return TextWarmupStatus::Failed;
            }
        }
        for (const auto& child : element->GetChildren())
            if (child->GetDfsParent() == element) pending.push_back({child.get(), node.Sheets, hidden});
        if (auto* mount = element->GetMountTarget(); mount && mount->GetDfsParent() == element)
            pending.push_back({mount, node.Sheets, hidden});
    }
    return waiting ? TextWarmupStatus::Pending : TextWarmupStatus::Ready;
}
} // namespace GameEngine
