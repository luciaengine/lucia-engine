#include "LocalizationPanel.h"
#include "LocalizationKeyListProvider.h"
#include "Localization/MessageCheck.h"
#include "Panels/ConfirmActionModal.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/InspectorSection.h"
#include "UI/PanelSearchBar.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIManager.h"
#include "Rendering/Text/FontAtlas.h"
#include "Core/Engine.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"
#include "Platform/Shell.h"
#include "Platform/WebFileDownload.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kSourceLocale = "en";
// The language list's last item, which opens the add-language field. Never a language tag:
// tags cannot start with '+'.
constexpr const char* kAddLanguageValue = "+add";
constexpr const char* kHidden = "hidden";
constexpr const char* kInvalidField = "value-box-invalid";


void SetShown(UIElement* element, bool shown)
{
    if (shown) element->RemoveClass(kHidden);
    else element->AddClass(kHidden);
}

// Shows `message` under `field` and marks the field invalid; an empty message clears both.
void ShowFieldProblem(UIElement* field, Label* messageLabel, const std::string& message)
{
    messageLabel->SetText(message);
    SetShown(messageLabel, !message.empty());
    if (message.empty()) field->RemoveClass(kInvalidField);
    else field->AddClass(kInvalidField);
}

// A key as the list and the entry title show it: its prefix up to the last '.', and its last
// segment. "menu.play" is {"menu.", "play"}; a key without a '.' is all last segment.
std::pair<std::string, std::string> SplitKey(const std::string& key)
{
    const auto split = key.rfind('.');
    const auto cut = split == std::string::npos ? 0 : split + 1;
    return {key.substr(0, cut), key.substr(cut)};
}

void BindKeyRow(UIElement* row, ListId, int index, IListDataProvider* provider)
{
    const auto* keys = static_cast<const LocalizationKeyListProvider*>(provider);
    if (!row || !keys || index < 0 || index >= keys->GetItemCount() || row->GetChildren().size() < 5)
        return;
    const auto& entry = keys->Rows()[static_cast<size_t>(index)];
    const auto& children = row->GetChildren();
    const auto [prefixText, lastText] = SplitKey(entry.Key);
    if (auto* prefix = dynamic_cast<Label*>(children[0].get())) prefix->SetText(prefixText);
    if (auto* last = dynamic_cast<Label*>(children[1].get())) last->SetText(lastText);
    row->SetTooltip(entry.Key);
    if (entry.Current) row->AddCustomState("current");
    else row->RemoveCustomState("current");
    SetShown(children[3].get(), entry.Id == keys->GetDirtyId());
    SetShown(children[4].get(), entry.Missing);
    // Only the key text dims, so the unapplied dot keeps its one look.
    for (int part = 0; part < 2; ++part)
    {
        if (entry.Pinned) children[part]->AddClass("localization-key-text-pinned");
        else children[part]->RemoveClass("localization-key-text-pinned");
    }
}

// "English source, character 4: a { is never closed. Close it with }, ..."
std::string DescribeProblem(const std::string& field, const Localization::MessageProblem& problem)
{
    std::string text = field;
    if (problem.Character > 0) text += ", character " + std::to_string(problem.Character);
    return text + ": " + problem.Problem + ". " + problem.Fix;
}

std::string JoinArguments(const std::vector<std::string>& names)
{
    std::string text;
    for (const auto& name : names) text += (text.empty() ? "{" : ", {") + name + "}";
    return text.empty() ? "none" : text;
}

std::string TranslationProblem(const std::string& translation, const std::string& source, const std::string& locale)
{
    const std::string field = "Translation (" + locale + ")";
    if (const auto problem = Localization::CheckMessage(translation, locale))
        return DescribeProblem(field, *problem);
    const auto wanted = Localization::MessageArgumentNames(source);
    const auto used = Localization::MessageArgumentNames(translation);
    if (wanted == used) return {};
    std::vector<std::string> extra;
    std::vector<std::string> missing;
    std::set_difference(used.begin(), used.end(), wanted.begin(), wanted.end(), std::back_inserter(extra));
    std::set_difference(wanted.begin(), wanted.end(), used.begin(), used.end(), std::back_inserter(missing));
    const std::string problem = !extra.empty()
        ? "uses " + JoinArguments(extra) + ", which the English source does not have"
        : "leaves out " + JoinArguments(missing) + " from the English source";
    return field + ": " + problem + ". Use the English source's arguments: " + JoinArguments(wanted) + ".";
}

// The preview's message arguments, or why the text in the field is not usable.
bool ParseArguments(const std::string& text, Localization::MessageArguments& arguments, std::string& problem)
{
    const auto json = nlohmann::json::parse(text.empty() ? "{}" : text, nullptr, false);
    if (json.is_discarded() || !json.is_object())
    {
        problem = "Arguments must be a JSON object, e.g. {\"count\": 3}.";
        return false;
    }
    for (const auto& [key, value] : json.items())
    {
        if (value.is_string()) arguments[key] = value.get<std::string>();
        else if (value.is_number_integer()) arguments[key] = value.get<int64_t>();
        else if (value.is_number()) arguments[key] = value.get<double>();
        else
        {
            problem = "Argument \"" + key + "\" must be text or a number, e.g. {\"" + key + "\": 3}.";
            return false;
        }
    }
    return true;
}

// The top of the caption over `field`: the nearest laid-out element before the field, or before
// the closest ancestor of the field that has one.
float CaptionTop(const UIElement* field)
{
    for (const UIElement* node = field; node->GetParent(); node = node->GetParent())
    {
        const auto& siblings = node->GetParent()->GetChildren();
        const auto self = std::find_if(siblings.begin(), siblings.end(),
                                       [node](const auto& sibling) { return sibling.get() == node; });
        for (auto before = self; before != siblings.begin();)
        {
            --before;
            if ((*before)->GetLayoutHeight() > 0.0f) return (*before)->GetLayoutY();
        }
    }
    return field->GetLayoutY();
}

std::string EntryText(const Localization::TranslationEntry& entry, const std::string& locale)
{
    const auto found = entry.Translations.find(locale);
    return found == entry.Translations.end() ? std::string{} : found->second;
}

// Preview arguments for a message: every argument its source declares, set to 2, which reads
// as a plural count and as text. Empty for a message without arguments.
std::string SampleArguments(const std::string& source)
{
    const auto names = Localization::MessageArgumentNames(source);
    if (names.empty()) return {};
    nlohmann::json json = nlohmann::json::object();
    for (const auto& name : names) json[name] = 2;
    return json.dump();
}

// Code points in UTF-8 `text`.
size_t CharacterCount(const std::string& text)
{
    return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](char byte)
    { return (static_cast<unsigned char>(byte) & 0xC0u) != 0x80u; }));
}

std::string JoinNames(const std::vector<std::string>& names)
{
    std::string text;
    for (const auto& name : names) text += (text.empty() ? "" : ", ") + name;
    return text;
}

// "Dutch (nl)": the language list names each language and its tag.
std::string LanguageLabel(const std::string& locale)
{
    return Localization::LanguageDisplayName(locale) + " (" + locale + ")";
}
}

LocalizationPanel::LocalizationPanel() : AssetBoundDockPanel("Localization", std::string(kAssetSourceAlias),
    kLayoutAssetPath, kStyleAssetPath) {}

LocalizationPanel::~LocalizationPanel() = default;

void LocalizationPanel::OnLayoutBound()
{
    auto* scroll = dynamic_cast<ScrollView*>(FindById("EntryScroll"));
    auto* content = FindById("EntryContent");
    if (!scroll || !content)
    { Logger::Log::Error("Localization layout requires the EntryScroll scroll view and its EntryContent."); return; }
    scroll->AddContent(scroll->TakeChild(content));
    m_CatalogName = dynamic_cast<Label*>(FindById("CatalogName"));
    m_KeysEmpty = dynamic_cast<Label*>(FindById("KeysEmpty"));
    m_EntryTitlePrefix = dynamic_cast<Label*>(FindById("EntryTitlePrefix"));
    m_EntryTitleLast = dynamic_cast<Label*>(FindById("EntryTitleLast"));
    m_KeyFixed = dynamic_cast<Label*>(FindById("KeyFixed"));
    m_KeyLabel = dynamic_cast<Label*>(FindById("KeyLabel"));
    m_TranslationLabel = dynamic_cast<Label*>(FindById("TranslationLabel"));
    m_KeyError = dynamic_cast<Label*>(FindById("KeyError"));
    m_SourceError = dynamic_cast<Label*>(FindById("SourceError"));
    m_TranslationError = dynamic_cast<Label*>(FindById("TranslationError"));
    m_ArgumentsError = dynamic_cast<Label*>(FindById("ArgumentsError"));
    m_NewLocaleError = dynamic_cast<Label*>(FindById("NewLocaleError"));
    m_Preview = dynamic_cast<Label*>(FindById("Preview"));
    m_Status = dynamic_cast<Label*>(FindById("Status"));
    m_Key = dynamic_cast<TextField*>(FindById("TranslationKey"));
    m_Language = dynamic_cast<TextField*>(FindById("NewLocale"));
    m_Arguments = dynamic_cast<TextField*>(FindById("Arguments"));
    m_Source = dynamic_cast<TextArea*>(FindById("Source"));
    m_Translation = dynamic_cast<TextArea*>(FindById("Translation"));
    m_Context = dynamic_cast<TextArea*>(FindById("Context"));
    m_Notes = dynamic_cast<TextArea*>(FindById("Notes"));
    m_Locales = dynamic_cast<Dropdown*>(FindById("Locales"));
    m_Keys = dynamic_cast<ListView*>(FindById("Keys"));
    m_MissingOnly = dynamic_cast<Checkbox*>(FindById("MissingOnly"));
    m_Pseudo = dynamic_cast<Checkbox*>(FindById("Pseudo"));
    m_Apply = dynamic_cast<Button*>(FindById("Apply"));
    m_DiscardButton = dynamic_cast<Button*>(FindById("Discard"));
    m_TranslationGroup = FindById("TranslationGroup");
    m_DraftBadge = dynamic_cast<Label*>(FindById("DraftBadge"));
    m_PreviewNote = dynamic_cast<Label*>(FindById("PreviewNote"));
    m_SourceNotes = dynamic_cast<Label*>(FindById("SourceNotes"));
    m_TranslationCount = dynamic_cast<Label*>(FindById("TranslationCount"));
    m_Body = FindById("LocalizationBody");
    m_Splitter = FindById("LocalizationSplitter");
    m_KeysPane = dynamic_cast<WeightedPane*>(FindById("KeysPane"));
    m_EntryPane = dynamic_cast<WeightedPane*>(FindById("EntryPane"));
    m_CollapsibleButtons.clear();
    for (const char* id : {"Discard", "Apply", "Import", "Export"})
        m_CollapsibleButtons.push_back(dynamic_cast<Button*>(FindById(id)));
    for (const char* id : {"Open", "Save", "Import", "Export", "AddLocale", "CancelLocale", "NewKey", "Discard", "Apply"})
        FindById(id)->RequestSubtreeStyleAssetPath("Editor/UI/controls/LocalizationButton.css", kAssetSourceAlias);
    m_AddLanguageRow = FindById("AddLanguageRow");
    BindKeyList();
    BindSearch();
    BindNotesSection();
    FindById("Open")->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Open(); });
    FindById("Save")->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Save(); });
    FindById("Import")->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Exchange(true); });
    FindById("Export")->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Exchange(false); });
    FindById("AddLocale")->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { AddLanguage(); });
    FindById("CancelLocale")->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { HideAddLanguage(); });
    FindById("NewKey")->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { NewKey(); });
    m_Apply->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ApplyEntry(); });
    m_DiscardButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Discard(); });
    m_Locales->SetOnValueChanged([this](const std::string& value) { OnLocaleChosen(value); });
    m_MissingOnly->SetOnValueChanged([this](bool) { RefreshKeys(); });
    m_Pseudo->SetOnValueChanged([this](bool) { Preview(); });
    m_Arguments->SetOnValueChanging([this](const std::string&) { Preview(); });
    m_Arguments->SetOnValueChanged([this](const std::string&) { Preview(); });
    const auto edited = [this](const std::string&) { OnDraftEdited(); };
    for (BaseField* field : std::initializer_list<BaseField*>{m_Key, m_Source, m_Translation, m_Context, m_Notes})
    {
        field->SetOnValueChanging(edited);
        field->SetOnValueChanged(edited);
        field->RegisterEventHandler(kEventFocusIn, [this, field](UIEvent&) { m_LastFocusedField = field; RevealInEntry(field); });
    }
    m_Arguments->RegisterEventHandler(kEventFocusIn, [this](UIEvent&) { RevealInEntry(m_Arguments); });
    RefreshLocales();
    ShowCatalogName();
    ShowEntry({});
    RefreshKeys();
    ShowState();
}

void LocalizationPanel::OnPostLayout()
{
    AssetBoundDockPanel::OnPostLayout();
    const float width = GetLayoutWidth();
    if (!m_Body || width <= 0.0f) return;
    // The classes change only when the width crosses a breakpoint, so the restyle they cause
    // settles in the next layout. Neither breakpoint depends on the classes it sets.
    ShowCompact(width < kCompactBelowWidth);
    if (const float stackBelow = StackBelowWidth(); stackBelow > 0.0f) ShowStacked(width < stackBelow);
}

float LocalizationPanel::StackBelowWidth() const
{
    UIManager* manager = GetOwnerManager();
    const ResolvedStyle* keys = manager ? manager->TryGetResolvedStyleFor(m_KeysPane) : nullptr;
    const ResolvedStyle* entry = manager ? manager->TryGetResolvedStyleFor(m_EntryPane) : nullptr;
    if (!keys || !entry || keys->Layout.MinWidth.Unit != StyleLength::UnitType::Px ||
        entry->Layout.MinWidth.Unit != StyleLength::UnitType::Px)
        return 0.0f;
    // The splitter's thin side, whichever way it currently runs.
    const float splitter = std::min(m_Splitter->GetLayoutWidth(), m_Splitter->GetLayoutHeight());
    return keys->Layout.MinWidth.Value + splitter + entry->Layout.MinWidth.Value;
}

void LocalizationPanel::ShowStacked(bool stacked)
{
    if (stacked == m_Stacked) return;
    m_Stacked = stacked;
    // Reflowing moves the fields; the one being edited is brought back into view after it.
    PostSafeAction([this] { RevealInEntry(m_LastFocusedField); });
    if (stacked)
    {
        // Stacked, the entry is as tall as its fields and the list takes the slack.
        m_KeysWeight = m_KeysPane->GetFlexWeight();
        m_EntryWeight = m_EntryPane->GetFlexWeight();
        m_KeysPane->SetFlexWeight(1.0f);
        m_EntryPane->SetFlexWeight(0.0f);
        m_Body->AddClass("localization-body-stacked");
        m_KeysPane->AddClass("localization-keys-stacked");
        m_EntryPane->AddClass("localization-entry-pane-stacked");
        m_Splitter->RemoveClass("row");
        m_Splitter->AddClass("col");
    }
    else
    {
        m_KeysPane->SetFlexWeight(m_KeysWeight);
        m_EntryPane->SetFlexWeight(m_EntryWeight);
        m_Body->RemoveClass("localization-body-stacked");
        m_KeysPane->RemoveClass("localization-keys-stacked");
        m_EntryPane->RemoveClass("localization-entry-pane-stacked");
        m_Splitter->RemoveClass("col");
        m_Splitter->AddClass("row");
    }
}

void LocalizationPanel::ShowCompact(bool compact)
{
    if (compact == m_Compact) return;
    m_Compact = compact;
    for (Button* button : m_CollapsibleButtons)
    {
        button->SetTooltip(compact ? button->GetText() : std::string{});
        if (compact) button->AddClass("localization-icon-compact");
        else button->RemoveClass("localization-icon-compact");
        for (const auto& child : button->GetChildren())
            if (child->HasClass("button-text"))
            {
                if (compact) child->AddClass("localization-button-label-hidden");
                else child->RemoveClass("localization-button-label-hidden");
            }
    }
}

void LocalizationPanel::RevealInEntry(UIElement* field)
{
    auto* scroll = dynamic_cast<ScrollView*>(FindById("EntryScroll"));
    if (!scroll || !field) return;
    // The field's caption above stays in view with it, and the view stops in the gap over the
    // caption (narrower than the entry's 3 px column gap), so no edge of the element above shows.
    // Below, a margin keeps the field's message in view.
    constexpr float kGapOverCaption = 2.0f;
    constexpr float kMessageMargin = 24.0f;
    const float viewTop = scroll->GetLayoutY();
    const float viewBottom = viewTop + scroll->GetViewportHeight();
    const float top = CaptionTop(field) - kGapOverCaption;
    const float bottom = field->GetLayoutY() + field->GetLayoutHeight() + kMessageMargin;
    // Either way the caption lands at the top of the view, on the gap above it.
    if (top < viewTop || bottom > viewBottom) scroll->ScrollBy(0.0f, top - viewTop);
}

void LocalizationPanel::BindNotesSection()
{
    // Context and notes sit in the Inspector's own section, collapsed until opened, so the
    // header, disclosure and spacing match the Inspector's.
    UIElement* host = FindById("NotesHost");
    UIElement* content = FindById("NotesContent");
    auto section = std::make_unique<InspectorSection>("Context and translator notes");
    section->SetShowEnabledToggle(false);
    section->SetCollapsed(true);
    section->GetContentRoot()->AddChild(content->GetParent()->TakeChild(content));
    host->AddChild(std::move(section));
}

std::unique_ptr<UIElement> LocalizationPanel::MakeKeyRow(ListId, IListDataProvider*)
{
    auto row = std::make_unique<UIElement>();
    auto* manager = GetOwnerManager();
    if (!manager || !manager->InstantiateLayoutChildrenFromAssetPath(
            row.get(), "localization:Editor/UI/panels/LocalizationKeyRow.uxml"))
        return {};
    return row;
}

void LocalizationPanel::BindKeyList()
{
    m_KeyProvider = std::make_unique<LocalizationKeyListProvider>();
    m_Keys->SetDataProvider(m_KeyProvider.get());
    m_Keys->SetFocusable(true);
    m_Keys->SetItemFactory([this](ListId id, IListDataProvider* provider) { return MakeKeyRow(id, provider); });
    m_Keys->SetItemBinder(&BindKeyRow);
    m_Keys->SetOnSelectionChanged([this](ListId id) { OnKeySelected(id); });
}

void LocalizationPanel::BindSearch()
{
    // The key filter is the shared editor search bar, built into its slot; it filters as typed.
    const auto refilter = [this](const std::string&) { RefreshKeys(); };
    PanelSearchBarBuilt search = BuildPanelSearchBar("LocalizationSearch", {}, refilter, refilter);
    search.RootPtr->SetPlaceholder("Filter keys");
    search.RootPtr->AddClass("localization-search");
    search.RootPtr->RequestSubtreeStyleAssetPath("Editor/UI/controls/LocalizationSearchBar.css", kAssetSourceAlias);
    m_Search = search.FieldPtr;
    FindById("SearchHost")->AddChild(std::move(search.Root));
}

Localization::TranslationEntry LocalizationPanel::DraftEntry() const
{
    Localization::TranslationEntry entry;
    if (!IsNewEntry())
        if (const auto found = m_Document.Entries().find(HashStringId(m_EditKey)); found != m_Document.Entries().end())
            entry = found->second;
    entry.Key = IsNewEntry() ? m_Key->GetValue() : m_EditKey;
    entry.Context = m_Context->GetValue();
    entry.Notes = m_Notes->GetValue();
    entry.Translations[kSourceLocale] = m_Source->GetValue();
    if (m_EditLocale != kSourceLocale)
    {
        if (m_Translation->GetValue().empty()) entry.Translations.erase(m_EditLocale);
        else entry.Translations[m_EditLocale] = m_Translation->GetValue();
    }
    return entry;
}

bool LocalizationPanel::DraftChanged() const
{
    if (IsNewEntry())
        return !m_Key->GetValue().empty() || !m_Source->GetValue().empty() || !m_Translation->GetValue().empty() ||
               !m_Context->GetValue().empty() || !m_Notes->GetValue().empty();
    const auto found = m_Document.Entries().find(HashStringId(m_EditKey));
    if (found == m_Document.Entries().end()) return false;
    const auto& entry = found->second;
    return entry.Context != m_Context->GetValue() || entry.Notes != m_Notes->GetValue() ||
        EntryText(entry, kSourceLocale) != m_Source->GetValue() ||
        (m_EditLocale != kSourceLocale && EntryText(entry, m_EditLocale) != m_Translation->GetValue());
}

LocalizationPanel::DraftProblems LocalizationPanel::FindDraftProblems() const
{
    DraftProblems problems;
    if (!DraftChanged()) return problems;
    const auto& key = m_Key->GetValue();
    if (IsNewEntry() && key.empty())
        problems.Key = "Enter a key, e.g. menu.play.";
    else if (IsNewEntry() && m_Document.Entries().contains(HashStringId(key)))
        problems.Key = "\"" + key + "\" already exists. Select it in the list to edit it.";
    const auto& source = m_Source->GetValue();
    if (source.empty())
        problems.Source = "English source: enter the text the game shows in English.";
    else if (const auto problem = Localization::CheckMessage(source, kSourceLocale))
        problems.Source = DescribeProblem("English source", *problem);
    if (m_EditLocale != kSourceLocale && !m_Translation->GetValue().empty() && problems.Source.empty())
        problems.Translation = TranslationProblem(m_Translation->GetValue(), source, m_EditLocale);
    return problems;
}

void LocalizationPanel::ShowDraftState()
{
    const bool changed = DraftChanged();
    const auto problems = FindDraftProblems();
    ShowFieldProblem(m_Key, m_KeyError, problems.Key);
    ShowFieldProblem(m_Source, m_SourceError, problems.Source);
    ShowFieldProblem(m_Translation, m_TranslationError, problems.Translation);
    m_Apply->SetEnabled(changed && !problems.Any());
    m_DiscardButton->SetEnabled(changed);
    // A key is typed for a new entry, and shown read-only once the entry exists.
    SetShown(m_Key, IsNewEntry());
    SetShown(m_KeyFixed, !IsNewEntry());
    m_KeyFixed->SetText(m_EditKey);
    m_KeyLabel->SetText(IsNewEntry() ? "Key" : "Key (fixed once the entry exists)");
    const auto [prefix, last] = IsNewEntry() ? std::pair<std::string, std::string>{{}, "New key"} : SplitKey(m_EditKey);
    m_EntryTitlePrefix->SetText(prefix);
    m_EntryTitleLast->SetText(last);
    SetShown(m_DraftBadge, changed);
    m_KeyProvider->SetDirtyId(changed && !IsNewEntry() ? HashStringId(m_EditKey) : 0);
    // While translating, the translator reads the context and notes under the source.
    std::string notes;
    if (!m_Context->GetValue().empty()) notes = "Context: " + m_Context->GetValue();
    if (!m_Notes->GetValue().empty()) notes += (notes.empty() ? "" : "\n") + std::string("Notes: ") + m_Notes->GetValue();
    m_SourceNotes->SetText(notes);
    SetShown(m_SourceNotes, m_EditLocale != kSourceLocale && !notes.empty());
    m_TranslationCount->SetText(std::to_string(CharacterCount(m_Translation->GetValue())) + " characters; English source " +
                                std::to_string(CharacterCount(m_Source->GetValue())));
}

void LocalizationPanel::OnDraftEdited()
{
    ShowDraftState();
    Preview();
    ShowState();
}

bool LocalizationPanel::ResolveDraft(const std::string& action)
{
    if (!DraftChanged()) return true;
    if (const auto problems = FindDraftProblems(); problems.Any())
    {
        const std::string name = IsNewEntry() ? (m_Key->GetValue().empty() ? std::string("the new key") : m_Key->GetValue()) : m_EditKey;
        ShowRefusal("Fix or discard the draft for " + name + " before " + action + ".");
        // The refusal sits on the status line; the field that holds the problem, already
        // outlined, comes into view with the focus.
        UIElement* field = !problems.Key.empty() ? static_cast<UIElement*>(m_Key)
                         : !problems.Source.empty() ? static_cast<UIElement*>(m_Source)
                                                    : static_cast<UIElement*>(m_Translation);
        RevealInEntry(field);
        if (auto* manager = GetOwnerManager()) manager->FocusElement(field);
        return false;
    }
    ApplyEntry();
    return true;
}

void LocalizationPanel::ApplyEntry()
{
    if (!DraftChanged() || FindDraftProblems().Any()) return;
    std::string error;
    auto entry = DraftEntry();
    const auto key = entry.Key;
    if (!m_Document.Update(std::move(entry), error)) { ShowRefusal(key + " was not applied: " + error); return; }
    m_EditKey = key;
    m_Dirty = true;
    RefreshLocales();
    RefreshKeys();
    ShowCatalogName();
    ShowDraftState();
    Preview();
    ShowState();
}

void LocalizationPanel::Discard()
{
    if (IsNewEntry()) { ShowEntry({}); ShowState(); return; }
    if (const auto found = m_Document.Entries().find(HashStringId(m_EditKey)); found != m_Document.Entries().end())
        ShowEntry(found->second);
    ShowState();
}

void LocalizationPanel::Open()
{
    if (!m_Dirty && !DraftChanged()) { ChooseCatalog(); return; }
    auto* confirm = dynamic_cast<ConfirmActionModal*>(FindById("LocalizationOpenConfirm"));
    if (!confirm)
    {
        auto modal = std::make_unique<ConfirmActionModal>();
        confirm = modal.get();
        confirm->SetId("LocalizationOpenConfirm");
        AddChild(std::move(modal));
    }
    confirm->SetOnConfirm([this] { ChooseCatalog(); });
    confirm->Show("Unsaved catalog", "Discard pending edits when opening another catalog?", "Choose catalog");
}

void LocalizationPanel::ChooseCatalog()
{
    const auto initial = m_Path.empty() ? EngineCore::GetInstance().GetWorkspaceRoot() / "Assets" : m_Path;
    const auto path = Platform::SelectFile(initial, "Localization catalog", "*.localization");
    if (path.empty()) return;
    std::ifstream input(path, std::ios::binary);
    if (!input) { Platform::ReleaseTransientFiles({path}); ShowRefusal("Cannot open " + path.filename().string() + "."); return; }
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    input.close();
    Platform::ReleaseTransientFiles({path});
    std::istringstream stream(bytes);
    std::string error;
    if (!m_Document.Load(stream, error)) { ShowRefusal(path.filename().string() + " is not a valid catalog: " + error); return; }
    m_Path = path; m_DiskContents = bytes; m_Dirty = false;
    m_EditLocale = kSourceLocale;
    m_EditKey.clear();
    RefreshLocales();
    ShowEntry({});
    RefreshKeys();
    ShowCatalogName();
    ShowState();
}

bool LocalizationPanel::Publish(const std::filesystem::path& path, const std::string& bytes)
{
    const auto temporary = FileSystem::MakeTemporarySiblingPath(path);
    std::ofstream output(temporary, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output || !FileSystem::PublishFile(temporary, path))
    {
        std::error_code ignored; std::filesystem::remove(temporary, ignored);
        ShowRefusal("Could not save " + path.string() + ". The original file is unchanged.");
        return false;
    }
    return true;
}

void LocalizationPanel::Save()
{
    if (!ResolveDraft("saving")) return;
#if defined(__EMSCRIPTEN__)
    std::ostringstream output; std::string error;
    if (!m_Document.Write(output, error)) { ShowRefusal(error); return; }
    const auto name = m_Path.empty() ? "Translations.localization" : m_Path.filename().string();
    if (!Platform::DownloadFile(name, output.str())) { ShowRefusal("Could not start the catalog download."); return; }
    m_Dirty = false;
    ShowCatalogName();
    ShowState();
#else
    auto path = m_Path;
    if (path.empty()) path = Platform::SaveFile("Translations.localization", "Localization catalog", "*.localization");
    if (path.empty()) return;
    if (path == m_Path)
    {
        std::ifstream current(path, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(current), {}};
        if (!current || bytes != m_DiskContents)
        { ShowRefusal("The catalog changed on disk. Open it again before saving so external edits are not overwritten."); return; }
    }
    std::ostringstream output; std::string error;
    if (!m_Document.Write(output, error)) { ShowRefusal(error); return; }
    if (!Publish(path, output.str())) return;
    m_Path = path; m_DiskContents = output.str(); m_Dirty = false;
    ShowCatalogName();
    ShowState();
#endif
}

void LocalizationPanel::OnLocaleChosen(std::string value)
{
    const auto& locales = m_Document.Locales();
    const auto current = std::find(locales.begin(), locales.end(), m_EditLocale);
    const int currentIndex = current == locales.end() ? 0 : static_cast<int>(current - locales.begin());
    if (value == kAddLanguageValue)
    {
        m_Locales->SetSelectedIndexWithoutNotify(currentIndex);
        ShowAddLanguage();
        return;
    }
    if (value == m_EditLocale) return;
    if (!ResolveDraft("switching languages")) { m_Locales->SetSelectedIndexWithoutNotify(currentIndex); return; }
    m_EditLocale = value;
    RefreshKeys();
    if (const auto found = m_Document.Entries().find(HashStringId(m_EditKey)); found != m_Document.Entries().end())
        ShowEntry(found->second);
    else
        ShowEntry({});
    ShowState();
}

void LocalizationPanel::ShowAddLanguage()
{
    SetShown(m_AddLanguageRow, true);
    ShowFieldProblem(m_Language, m_NewLocaleError, "");
    if (auto* manager = GetOwnerManager()) manager->FocusElement(m_Language);
}

void LocalizationPanel::HideAddLanguage()
{
    m_Language->SetValueWithoutNotify("");
    ShowFieldProblem(m_Language, m_NewLocaleError, "");
    SetShown(m_AddLanguageRow, false);
}

void LocalizationPanel::AddLanguage()
{
    const auto tag = m_Language->GetValue();
    if (tag.empty()) return;
    std::string error;
    Localization::LocalizationCatalog language;
    if (!language.SetLocale(tag, error))
    {
        ShowFieldProblem(m_Language, m_NewLocaleError,
                         "\"" + tag + "\" is not a language tag. Use a language tag such as fr, pt-BR or zh-Hant.");
        return;
    }
    const auto added = language.GetLocale();
    if (std::find(m_Document.Locales().begin(), m_Document.Locales().end(), added) != m_Document.Locales().end())
    {
        ShowFieldProblem(m_Language, m_NewLocaleError, added + " is already in the catalog. Choose it in the language list.");
        return;
    }
    if (!ResolveDraft("adding a language")) return;
    if (!m_Document.AddLocale(added, error)) { ShowRefusal(error); return; }
    m_Dirty = true;
    HideAddLanguage();
    m_EditLocale = added;
    RefreshLocales();
    RefreshKeys();
    if (const auto found = m_Document.Entries().find(HashStringId(m_EditKey)); found != m_Document.Entries().end())
        ShowEntry(found->second);
    else
        ShowEntry({});
    ShowCatalogName();
    ShowState();
}

void LocalizationPanel::NewKey()
{
    if (!ResolveDraft("starting a new key")) return;
    m_EditKey.clear();
    ShowEntry({});
    // Rebuilding the rows drops the list's selection; nothing is reselected for a new key.
    RefreshKeys();
    if (auto* manager = GetOwnerManager()) manager->FocusElement(m_Key);
    ShowState();
}

void LocalizationPanel::RefreshLocales()
{
    std::vector<Dropdown::Option> options;
    int selected = 0;
    for (const auto& locale : m_Document.Locales())
    {
        if (locale == m_EditLocale) selected = static_cast<int>(options.size());
        if (locale == kSourceLocale) { options.push_back({locale, LanguageLabel(locale) + " · source", {}, {}}); continue; }
        const auto missing = m_Document.Missing(locale).size();
        options.push_back({locale, LanguageLabel(locale) + (missing == 0 ? " · complete" : " · " + std::to_string(missing) + " missing"), {}, {}});
    }
    options.push_back({kAddLanguageValue, "+ Add language…", {}, {}});
    m_Locales->SetOptions(options, selected);
}

void LocalizationPanel::RefreshKeys()
{
    // Sorted by id, so each row looks itself up by binary search.
    const auto missing = m_Document.Missing(m_EditLocale);
    std::vector<LocalizationKeyListProvider::Row> rows;
    for (const auto id : m_Document.FindKeys(m_Search->GetValue(), m_EditLocale, m_MissingOnly->IsChecked()))
        rows.push_back({id, m_Document.Entries().at(id).Key, std::binary_search(missing.begin(), missing.end(), id), false, !IsNewEntry() && id == HashStringId(m_EditKey)});
    // The edited entry stays listed, dimmed, while the filter would hide it.
    const StringId editId = IsNewEntry() ? StringId{0} : HashStringId(m_EditKey);
    if (editId && std::none_of(rows.begin(), rows.end(), [editId](const auto& row) { return row.Id == editId; }))
        rows.insert(rows.begin(), {editId, m_EditKey, std::binary_search(missing.begin(), missing.end(), editId), true, true});
    const bool empty = rows.empty();
    const bool onlyPinned = rows.size() == 1 && rows.front().Pinned;
    m_KeyProvider->SetRows(std::move(rows));
    m_Keys->RefreshFromProvider();
    // A rebuild drops the list's selection, and inside an event it runs later, posted; the
    // row of the entry being edited is selected again after it.
    PostSafeAction([this] { ShowSelectedKey(); });
    SetShown(m_Keys, !empty);
    SetShown(m_KeysEmpty, empty || onlyPinned);
    m_KeysEmpty->SetText(m_Document.Entries().empty()
        ? "No keys yet. Choose New key, then type a key and its English source."
        : onlyPinned ? "No keys match. " + m_EditKey + " is shown because " +
                           (DraftChanged() ? "it has unapplied changes." : "it is open in the editor.")
                     : "No keys match the filter.");
}

void LocalizationPanel::OnKeySelected(ListId id)
{
    // ListIds are row index + 1; 0 is no selection.
    const int index = static_cast<int>(id) - 1;
    if (m_SyncingSelection || index < 0 || index >= m_KeyProvider->GetItemCount()) return;
    // Copied: applying the draft below rebuilds the rows.
    const StringId keyId = m_KeyProvider->Rows()[static_cast<size_t>(index)].Id;
    if (!IsNewEntry() && keyId == HashStringId(m_EditKey)) return;
    if (!ResolveDraft("switching keys")) { ShowSelectedKey(); return; }
    const auto found = m_Document.Entries().find(keyId);
    if (found == m_Document.Entries().end()) return;
    ShowEntry(found->second);
    RefreshKeys();
    ShowState();
}

void LocalizationPanel::ShowSelectedKey()
{
    const int index = IsNewEntry() ? -1 : m_KeyProvider->IndexOf(HashStringId(m_EditKey));
    m_SyncingSelection = true;
    m_Keys->SetSelectedIndex(index, true);
    m_SyncingSelection = false;
}

void LocalizationPanel::ShowEntry(const Localization::TranslationEntry& entry)
{
    // Arguments belong to one message: another key starts from samples of its own arguments,
    // while showing the same key again (a language switch, a discard) keeps the edited ones.
    if (entry.Key.empty() || entry.Key != m_EditKey)
        m_Arguments->SetValueWithoutNotify(SampleArguments(EntryText(entry, kSourceLocale)));
    m_EditKey = entry.Key;
    m_Key->SetValueWithoutNotify(entry.Key);
    m_Source->SetValueWithoutNotify(EntryText(entry, kSourceLocale));
    m_Translation->SetValueWithoutNotify(m_EditLocale == kSourceLocale ? std::string{} : EntryText(entry, m_EditLocale));
    m_Context->SetValueWithoutNotify(entry.Context);
    m_Notes->SetValueWithoutNotify(entry.Notes);
    // The source language is edited in the English source field.
    SetShown(m_TranslationGroup, m_EditLocale != kSourceLocale);
    m_TranslationLabel->SetText(LanguageLabel(m_EditLocale) + " translation; empty falls back to English");
    ShowDraftState();
    Preview();
}

void LocalizationPanel::Preview()
{
    m_Preview->RemoveClass("localization-preview-error");
    SetShown(m_PreviewNote, false);
    std::string problem;
    Localization::MessageArguments arguments;
    const bool argumentsValid = ParseArguments(m_Arguments->GetValue(), arguments, problem);
    ShowFieldProblem(m_Arguments, m_ArgumentsError, argumentsValid ? std::string{} : problem);
    const auto draft = DraftEntry();
    if (draft.Key.empty() && m_Source->GetValue().empty())
    { m_Preview->SetText("Select a key, or choose New key."); return; }
    // The reason sits under the field it names; the preview only says which field holds it.
    const auto problems = FindDraftProblems();
    const char* blocking = !problems.Source.empty() ? "No preview until the English source is fixed."
                         : !problems.Translation.empty() ? "No preview until the translation is fixed."
                         : !argumentsValid ? "No preview until the arguments are valid JSON." : nullptr;
    if (blocking)
    {
        m_Preview->AddClass("localization-preview-error");
        m_Preview->SetText(blocking);
        return;
    }
    std::vector<std::string> unset;
    for (const auto& name : Localization::MessageArgumentNames(EntryText(draft, kSourceLocale)))
        if (!arguments.contains(name)) unset.push_back(name);
    if (!unset.empty())
    {
        m_Preview->AddClass("localization-preview-error");
        m_Preview->SetText("The preview needs a value for " + JoinArguments(unset) + ": add it to Arguments, e.g. {\"" +
                           unset.front() + "\": 2}.");
        return;
    }
    // A one-entry document holding the draft in every language of the catalog resolves the
    // text and its fallback at any catalog size.
    Localization::LocalizationDocument document;
    std::string error;
    for (const auto& locale : m_Document.Locales())
        if (locale != kSourceLocale) document.AddLocale(locale, error);
    auto previewEntry = draft;
    if (previewEntry.Key.empty()) previewEntry.Key = "preview";
    const auto key = previewEntry.Key;
    std::shared_ptr<Localization::LocalizationCatalog> catalog;
    if (document.Update(std::move(previewEntry), error)) catalog = document.CreatePreview(m_EditLocale, error);
    if (!catalog) { m_Preview->AddClass("localization-preview-error"); m_Preview->SetText("No preview: " + error); return; }
    const auto text = catalog->Resolve(HashStringId(key), "", arguments, &error);
    if (!error.empty()) { m_Preview->AddClass("localization-preview-error"); m_Preview->SetText("No preview: " + error); return; }
    const auto shown = m_Pseudo->IsChecked() ? Localization::Pseudolocalize(text) : text;
    m_Preview->SetText(shown);
    std::string note;
    if (m_EditLocale != kSourceLocale && EntryText(draft, m_EditLocale).empty())
        note = "No " + m_EditLocale + " translation yet; showing the English source.";
    else if (const auto categories = Localization::MissingPluralCategories(EntryText(draft, m_EditLocale), m_EditLocale);
             m_EditLocale != kSourceLocale && !categories.empty())
        note = "The " + m_EditLocale + " plural has no case for " + JoinNames(categories) +
               ", so those counts use other {...}; add the cases, e.g. " + categories.front() + " {...}.";
    if (!PreviewFontRenders(shown))
        note += std::string(note.empty() ? "" : " ") +
                "The editor font has no glyphs for some of this text, so they show blank; the text is stored correctly.";
    m_PreviewNote->SetText(note);
    SetShown(m_PreviewNote, !note.empty());
}

bool LocalizationPanel::PreviewFontRenders(const std::string& text) const
{
    UIManager* manager = GetOwnerManager();
    const ResolvedStyle* style = manager ? manager->TryGetResolvedStyleFor(m_Preview) : nullptr;
    Rendering::Text::FontAtlas* font = style ? manager->ResolveFontForStyle(*style) : nullptr;
    return !font || font->RendersAllGlyphs(text);
}

void LocalizationPanel::ShowCatalogName()
{
    const auto name = m_Path.empty() ? std::string("New catalog") : m_Path.filename().string();
    m_CatalogName->SetText(m_Dirty ? name + " (unsaved changes)" : name);
}

void LocalizationPanel::ShowState()
{
    m_Status->RemoveClass("localization-status-error");
    const auto total = m_Document.Entries().size();
    std::string state = std::to_string(total) + (total == 1 ? " key" : " keys");
    if (m_EditLocale != kSourceLocale)
        state += ", " + std::to_string(total - m_Document.Missing(m_EditLocale).size()) + " of " +
                 std::to_string(total) + " translated into " + m_EditLocale;
    if (m_Document.Locales().size() == 1) state += ", no target language yet";
    if (DraftChanged()) state += ", changes not applied";
    state += m_Dirty ? ", unsaved" : (m_Path.empty() ? "" : ", saved");
    m_Status->SetText(state + ".");
}

void LocalizationPanel::ShowRefusal(const std::string& text)
{
    m_Status->AddClass("localization-status-error");
    m_Status->SetText(text);
}

void LocalizationPanel::Exchange(bool import)
{
    if (!ResolveDraft(import ? "importing a PO file" : "exporting a PO file")) return;
    const auto locale = m_EditLocale;
#if defined(__EMSCRIPTEN__)
    if (!import)
    {
        std::ostringstream output; std::string error;
        if (!m_Document.ExportPo(output, locale, error)) { ShowRefusal(error); return; }
        if (!Platform::DownloadFile(locale + ".po", output.str())) ShowRefusal("Could not start the PO download.");
        else ShowState();
        return;
    }
#endif
    const auto path = import ? Platform::SelectFile(m_Path.parent_path(), "PO translation", "*.po")
                             : Platform::SaveFile(m_Path.parent_path() / (locale + ".po"), "PO translation", "*.po");
    if (path.empty()) return;
    std::string error;
    if (import)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) { Platform::ReleaseTransientFiles({path}); ShowRefusal("Cannot open " + path.filename().string() + "."); return; }
        const bool imported = m_Document.ImportPo(input, locale, error);
        input.close();
        Platform::ReleaseTransientFiles({path});
        if (!imported) { ShowRefusal(path.filename().string() + " was not imported: " + error); return; }
        m_Dirty = true;
        RefreshLocales();
        RefreshKeys();
        if (const auto found = m_Document.Entries().find(HashStringId(m_EditKey)); found != m_Document.Entries().end())
            ShowEntry(found->second);
        ShowCatalogName();
        ShowState();
    }
    else
    {
        std::ostringstream output;
        if (!m_Document.ExportPo(output, locale, error)) { ShowRefusal(error); return; }
        if (Publish(path, output.str())) ShowState();
    }
}
}
