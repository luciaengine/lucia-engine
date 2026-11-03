#pragma once
#include "UI/AssetBoundDockPanel.h"
#include "Localization/LocalizationDocument.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace GameEngine
{
class Button;
class Checkbox;
class ConfirmActionModal;
class Dropdown;
class Label;
class ListView;
struct IListDataProvider;
class TextArea;
class TextField;
class UIElement;
class WeightedPane;
using ListId = uint64_t;
namespace Editor
{
class LocalizationKeyListProvider;

// Edits a .localization catalog: its keys with their English source, context and translator
// notes, one target language's translations, PO exchange for translators and a live preview
// of the entry being edited. Layout and style ship with the localization package:
// Assets/Editor/UI/panels/LocalizationPanel.{uxml,css}, mounted as "localization".
//
// The entry fields are a draft. Apply changes commits it; switching key or language, starting
// a new key, adding a language, saving and PO exchange commit a valid draft first and are
// refused, with the reason, while it is invalid. Filtering the list never commits or refuses:
// the edited entry stays listed. Discard changes reverts the draft. A key is fixed once its
// entry exists.
class LocalizationPanel final : public AssetBoundDockPanel
{
public:
    static constexpr const char* kPanelId = "Localization";
    static constexpr const char* kAssetSourceAlias = "localization";
    static constexpr const char* kLayoutAssetPath = "Editor/UI/panels/LocalizationPanel.uxml";
    static constexpr const char* kStyleAssetPath = "Editor/UI/panels/LocalizationPanel.css";
    // The editor-cascade sheet that gives the panel's tab and menu rows their icon class.
    static constexpr const char* kChromeStyleAssetPath = "Editor/UI/LocalizationPanelChrome.css";
    static constexpr const char* kTabIconClass = "localization-tab-icon";
    LocalizationPanel();
    ~LocalizationPanel() override;
private:
    // What is wrong with the draft, field by field; empty strings for valid fields.
    struct DraftProblems
    {
        std::string Key;
        std::string Source;
        std::string Translation;
        bool Any() const { return !Key.empty() || !Source.empty() || !Translation.empty(); }
    };

    void OnLayoutBound() override;
    void OnPostLayout() override;
    // The panel width below which the key list sits above the entry: the two panes' minimum
    // widths from the stylesheet plus the splitter. 0 until the styles have resolved.
    float StackBelowWidth() const;
    void ShowStacked(bool stacked);
    // Narrow: the entry's and the PO buttons show their icon only, with the label as a tooltip.
    void ShowCompact(bool compact);
    // Scrolls the entry so `field`, its caption and its message are in view.
    void RevealInEntry(UIElement* field);
    // Whether the preview's font, with its fallbacks, draws every character of `text`.
    bool PreviewFontRenders(const std::string& text) const;
    std::unique_ptr<UIElement> MakeKeyRow(ListId id, IListDataProvider* provider);
    void BindKeyList();
    void BindSearch();
    void BindNotesSection();
    void Open();
    void ChooseCatalog();
    void Save();
    void Exchange(bool import);
    // By value: applying a draft rebuilds the language list that holds the chosen value.
    void OnLocaleChosen(std::string value);
    void ShowAddLanguage();
    void HideAddLanguage();
    void AddLanguage();
    void NewKey();
    void Discard();
    void RefreshLocales();
    void RefreshKeys();
    void OnKeySelected(ListId id);
    void ShowSelectedKey();
    void ShowEntry(const Localization::TranslationEntry& entry);
    // Commits a changed, valid draft. With a changed, invalid draft it reports that `action`
    // was refused and returns false.
    bool ResolveDraft(const std::string& action);
    void ApplyEntry();
    void OnDraftEdited();
    bool IsNewEntry() const { return m_EditKey.empty(); }
    Localization::TranslationEntry DraftEntry() const;
    bool DraftChanged() const;
    DraftProblems FindDraftProblems() const;
    // Shows the draft's problems beside their fields and enables Apply and Discard to match.
    void ShowDraftState();
    void Preview();
    void ShowCatalogName();
    // The status line: the catalog's state, or why an action was refused.
    void ShowState();
    void ShowRefusal(const std::string& text);
    bool Publish(const std::filesystem::path& path, const std::string& bytes);

    Localization::LocalizationDocument m_Document;
    // Logical px: a panel narrower than this shows the buttons that collapse as icons only.
    static constexpr float kCompactBelowWidth = 400.0f;
    std::unique_ptr<LocalizationKeyListProvider> m_KeyProvider;
    bool m_Stacked = false;
    bool m_Compact = false;
    // The list's and the entry's weights side by side, restored when the panes stop stacking.
    float m_KeysWeight = 0.0f;
    float m_EntryWeight = 0.0f;
    // The entry field that last took focus, revealed again after the layout reflows.
    UIElement* m_LastFocusedField = nullptr;
    // Set while the panel selects a row itself, so the list's notification is not an edit.
    bool m_SyncingSelection = false;
    std::filesystem::path m_Path;
    std::string m_DiskContents;
    bool m_Dirty = false;
    // The target language and the entry the fields were filled from; empty key for a new entry.
    std::string m_EditLocale = "en";
    std::string m_EditKey;
    Label* m_CatalogName = nullptr;
    Label* m_KeysEmpty = nullptr;
    Label* m_EntryTitlePrefix = nullptr;
    Label* m_EntryTitleLast = nullptr;
    Label* m_KeyFixed = nullptr;
    Label* m_KeyLabel = nullptr;
    Label* m_TranslationLabel = nullptr;
    Label* m_KeyError = nullptr;
    Label* m_SourceError = nullptr;
    Label* m_TranslationError = nullptr;
    Label* m_ArgumentsError = nullptr;
    Label* m_NewLocaleError = nullptr;
    Label* m_Preview = nullptr;
    Label* m_Status = nullptr;
    TextField* m_Key = nullptr;
    TextField* m_Language = nullptr;
    TextField* m_Search = nullptr;
    TextField* m_Arguments = nullptr;
    TextArea* m_Source = nullptr;
    TextArea* m_Translation = nullptr;
    TextArea* m_Context = nullptr;
    TextArea* m_Notes = nullptr;
    Dropdown* m_Locales = nullptr;
    ListView* m_Keys = nullptr;
    Checkbox* m_MissingOnly = nullptr;
    Checkbox* m_Pseudo = nullptr;
    Button* m_Apply = nullptr;
    Button* m_DiscardButton = nullptr;
    UIElement* m_TranslationGroup = nullptr;
    UIElement* m_AddLanguageRow = nullptr;
    Label* m_DraftBadge = nullptr;
    Label* m_PreviewNote = nullptr;
    // The entry's context and notes, read-only under the source while translating.
    Label* m_SourceNotes = nullptr;
    Label* m_TranslationCount = nullptr;
    UIElement* m_Body = nullptr;
    UIElement* m_Splitter = nullptr;
    WeightedPane* m_KeysPane = nullptr;
    WeightedPane* m_EntryPane = nullptr;
    std::vector<Button*> m_CollapsibleButtons;
};
}
}
