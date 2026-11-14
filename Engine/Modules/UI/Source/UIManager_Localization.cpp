#include "UI/UIManager.h"
#include "UIManager/UIManager_Internal.h"
#include "UI/Controls/Label.h"

namespace GameEngine
{
void UIManager::RefreshLocalizedText()
{
    SetTextLanguage(m_LocalizationCatalog ? m_LocalizationCatalog->GetLocale() : "und");
    // The ownership index includes detached and mounted elements, without duplicate visits.
    for (const auto& [id, element] : m_ElementsByInstanceId)
        if (auto* label = dynamic_cast<Label*>(element)) label->RefreshLocalizedText();
}

void UIManager::SetLocalizationCatalog(std::shared_ptr<Localization::LocalizationCatalog> catalog)
{
    m_LocalizationSubscription.Reset();
    m_LocalizationCatalog = std::move(catalog);
    if (m_LocalizationCatalog)
        m_LocalizationSubscription = m_LocalizationCatalog->Changed.Subscribe([this]() { RefreshLocalizedText(); });
    RefreshLocalizedText();
}
} // namespace GameEngine
