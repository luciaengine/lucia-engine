#include "UI/UIManager.h"
#include "UIManager/UIManager_Internal.h"
#include "UI/Controls/Label.h"

#include "Localization/LocalizationCatalog.h"

namespace GameEngine
{
void UIManager::RefreshLocalizedText()
{
    // The ownership index includes detached and mounted elements, without duplicate visits.
    for (const auto& [id, element] : m_ElementsByInstanceId)
        if (auto* label = dynamic_cast<Label*>(element)) label->RefreshLocalizedText();
}

void UIManager::SetLocalizationCatalog(std::shared_ptr<Localization::LocalizationCatalog> catalog)
{
    m_LocalizationSubscription.Reset();
    m_LocalizationCatalog = std::move(catalog);
    SetTextLanguage(m_LocalizationCatalog ? m_LocalizationCatalog->GetLocale() : "und");
    if (m_LocalizationCatalog)
        m_LocalizationSubscription = m_LocalizationCatalog->Changed.Subscribe([this]() { RefreshLocalizedText(); });
    RefreshLocalizedText();
}
} // namespace GameEngine
