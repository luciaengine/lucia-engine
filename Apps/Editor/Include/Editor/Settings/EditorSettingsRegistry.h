#pragma once

#include "ECS/ModuleRegistration.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace GameEngine
{
class UIElement;

namespace Editor
{

// Which settings-tree root a registered category hangs under. Only the roots
// the current registrants need; extend on contact.
enum class SettingsCategoryGroup : uint32_t
{
    UserSettings,
    ProjectSettings,
    // Renders inline on the Version Control page itself, above the provider
    // list — no tree child of its own. For settings that are provider-neutral
    // rather than owned by one integration, and therefore apply to all of them.
    VersionControl,
    // Renders inline on the UI > Appearance page, after the hand-built rows —
    // no tree child of its own.
    UIAppearance,
    // Tree child of the hardcoded UI page, composed with Appearance / Font /
    // Trees / … rather than replacing them. Unlike UIAppearance, these get
    // their own settings-tree row.
    UI,
};

// One settings row, declared as data. Exactly one control alternative is
// active; the SettingsPanel renders it with the same row builders (label
// column, tooltip, double-click-to-reset, undo, preference persistence) used
// by the built-in categories.
struct SettingsFieldDescriptor
{
    std::string Label;          // row label, e.g. "Keep Universal Search Open"
    std::string Tooltip;        // full wording when Label is shortened; optional
    std::string SearchKeywords; // extra lowercase keywords for panel + palette search

    // Optional subsection break emitted immediately above this row, for pages
    // whose rows fall into named groups ("Steam Deck install", "Web export").
    // The category Title already heads the page, so leave both empty for a page
    // that is one flat list. SectionDescription explains the group and renders
    // as an info card under the header; a name belongs in SectionHeader.
    std::string SectionHeader;
    std::string SectionDescription;

    // Optional explanation shown as a collapsible info card under the row.
    std::string InfoCard;

    // Optional. The row, its section break and its info card are hidden while
    // this returns false. Re-evaluated after layout and after every toggle or
    // dropdown change on the page, so a toggle can pick which rows below it
    // apply, and a condition outside the page (a panel becoming the active
    // tab) takes effect without a rebuild. Called often: keep it cheap.
    std::function<bool()> IsVisible;

    // Editor preferences key ("onlineAssets.polyHaven"). When set, the panel
    // loads the initial value from Preferences.json and persists every change
    // (Editor::OpenEditorPreferences). Leave empty when the owning settings
    // singleton persists itself — then Get() supplies the initial value.
    std::string PrefKey;

    struct ToggleField
    {
        bool DefaultValue = false;
        std::function<bool()> Get;     // optional when PrefKey is set
        // Change callback. Also fired once with the initial value when the row
        // is built, so enable/disable reactions apply on page open.
        //
        // That build-time call passes Get()'s own value back, so a Set that
        // persists MUST return without writing when the value it is handed
        // already matches what is stored: opening a page is not an edit, and a
        // write there rewrites the user's project file behind their back.
        std::function<void(bool)> Set;
    };
    struct SliderField
    {
        float DefaultValue = 0.0f;
        // The row seeds itself from Get() clamped to [MinValue, MaxValue], so
        // this range must admit every value the backing store accepts. A
        // narrower widget shows a value the file does not hold, and the next
        // commit persists that clamp over the user's setting.
        float MinValue = 0.0f;
        float MaxValue = 1.0f;
        float Step = 0.0f;
        std::function<float()> Get;
        // Live feedback while the thumb or the label is dragged: one call per
        // drag tick, so it must not persist. Optional; without it nothing
        // changes until the commit.
        std::function<void(float)> Preview;
        // The commit: thumb or label release, value-field entry, double-click
        // reset, undo and redo. Never fired at build time: the value the row
        // opens with is already live, and a Preview may have left the live
        // value equal to the one being committed, so Set persists without
        // comparing against it.
        std::function<void(float)> Set;
    };
    // A typed number without a slider: a value field whose label drags it.
    // Preview and Set follow SliderField.
    struct NumberField
    {
        float DefaultValue = 0.0f;
        float MinValue = std::numeric_limits<float>::lowest();
        float MaxValue = std::numeric_limits<float>::max();
        std::function<float()> Get; // optional when PrefKey is set
        std::function<void(float)> Preview;
        std::function<void(float)> Set;
    };
    // NumberField for whole numbers.
    struct IntegerField
    {
        int DefaultValue = 0;
        int MinValue = std::numeric_limits<int>::min();
        int MaxValue = std::numeric_limits<int>::max();
        std::function<int()> Get; // optional when PrefKey is set
        std::function<void(int)> Preview;
        std::function<void(int)> Set;
    };
    struct DropdownField
    {
        struct Option
        {
            std::string Value; // what Get returns and Set is handed
            std::string Label; // what the row shows; Value when empty
        };

        std::vector<std::string> Options; // labels double as stored values
        // Options that are not known until the page opens (asset lists), or
        // whose stored value differs from what the row shows. Wins over
        // Options, and is re-read every time the page is built.
        std::function<std::vector<Option>()> OptionsProvider;
        std::string DefaultValue;
        std::function<std::string()> Get;
        std::function<void(const std::string&)> Set;
    };
    struct ColorField
    {
        bool ShowHexField = true;
        std::function<void(uint32_t)> Preview;
        uint32_t DefaultArgb = 0xFF000000u;
        std::function<uint32_t()> Get;
        std::function<void(uint32_t)> Set;
    };
    // Toggle that enables a look, with a color swatch on the same row (info-card
    // background / outline). SetColor may be invoked for live preview while the
    // picker is open; SetEnabled follows the ToggleField build-time contract.
    struct ToggleColorField
    {
        bool DefaultEnabled = true;
        uint32_t DefaultArgb = 0xFF000000u;
        std::function<bool()> GetEnabled;
        std::function<void(bool)> SetEnabled;
        std::function<uint32_t()> GetColor;
        // persist=false is live picker preview / cancel restore; true commits.
        std::function<void(uint32_t /*argb*/, bool /*persist*/)> SetColor;
    };
    struct StringField
    {
        std::string DefaultValue;
        std::function<std::string()> Get; // optional when PrefKey is set
        // Fired on edit and on focus-out, never at build time — unlike the
        // toggle and slider rows, a text row has nothing to react to on open.
        std::function<void(const std::string&)> Set;
    };
    // A filesystem path: a StringField plus a Browse button that opens the
    // native picker for Kind and writes the chosen path back through Set.
    struct PathField
    {
        enum class Kind : uint8_t
        {
            Directory,
            File,
        };

        Kind PathKind = Kind::Directory;
        std::string DefaultValue;
        std::function<std::string()> Get; // optional when PrefKey is set
        std::function<void(const std::string&)> Set;
    };
    // A row that performs an action instead of holding a value, so it has no
    // Get/Set and no PrefKey.
    struct ButtonField
    {
        std::string ButtonText;
        std::function<void()> OnClick;
    };
    // Escape hatch for a row the declarative controls cannot express: drop
    // targets, editable lists, anything owning its own layout and persistence.
    // The panel hosts what CreateRow returns and touches nothing inside it.
    struct CustomField
    {
        std::function<std::unique_ptr<UIElement>()> CreateRow;
    };

    std::variant<ToggleField, SliderField, NumberField, IntegerField, DropdownField, ColorField,
                 ToggleColorField, StringField, PathField, ButtonField, CustomField>
        Control = ToggleField{};
};

// What a system or package registers to add a category page to the Settings
// panel: a tree node under Group, a content page built from Fields, and
// search entries for the panel and the universal-search palette.
struct SettingsCategoryDescriptor
{
    // Stable id and replace-forward key, e.g. "tooltips", "onlineAssets".
    // Convention: matches the PrefKey prefix of the category's fields.
    std::string CategoryId;
    std::string Title;       // tree label + content header, e.g. "Tooltips"
    SettingsCategoryGroup Group = SettingsCategoryGroup::UserSettings;
    // CategoryId of a registered category to nest under. Empty means the page
    // sits directly under Group's root. Registration order does not matter: a
    // child that names a parent nobody has registered yet falls back to the
    // group root, and moves under the parent as soon as it appears.
    std::string ParentCategoryId;
    std::string Description; // optional trailing info card explaining the page
    // Keeps that card on screen when explanatory cards are globally
    // hidden. For the page that owns the switch; leave false everywhere else.
    bool DescriptionAlwaysVisible = false;
    std::string TreeRowClass; // optional Settings tree icon/style class
    std::string SearchKeywords; // extra lowercase keywords for category-level search

    std::vector<SettingsFieldDescriptor> Fields;
    // Optional custom page for settings that need richer UI than declarative rows.
    // When present, the SettingsPanel hosts the returned element directly.
    std::function<std::unique_ptr<UIElement>()> CreateContent;
    // Invoked on a working copy each time the SettingsPanel builds the page so
    // live lists (node type colors) can refresh after late registry Ensure.
    std::function<void(SettingsCategoryDescriptor&)> PrepareFields;
};

// Registration is main-thread only (module loads + editor startup), matching
// the other editor registries. Module lifetime is loud no-unload: a module
// rebuild re-registers under the same CategoryId and the NEW descriptor wins
// in place (replace-forward), so snapshot indices — which the SettingsPanel
// maps to tree ids — stay stable for the whole session.
class EditorSettingsRegistry
{
  public:
    static EditorSettingsRegistry& Get();

    void RegisterCategory(SettingsCategoryDescriptor descriptor);

    // Descriptors in registration order (replace-forward preserves position).
    std::vector<SettingsCategoryDescriptor> Snapshot() const;
    bool TryGet(std::string_view categoryId, SettingsCategoryDescriptor& outDescriptor) const;

    // Fired on every registration. Setting the observer replays existing
    // registrations so attach order never matters (the SettingsPanel is
    // constructed before package modules load at project open).
    using RegistrationObserver = std::function<void(const SettingsCategoryDescriptor&)>;
    void SetRegistrationObserver(RegistrationObserver observer);

    // C12 editor-kind unload-refusal diagnostics: append a description of every
    // category attributed to `moduleId` (field closures are module code and pin
    // its images mapped).
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

  private:
    EditorSettingsRegistry() = default;

    struct Entry
    {
        SettingsCategoryDescriptor Descriptor;
        ECS::ModuleRegistrationStamp Module;
    };

    std::vector<Entry> m_Categories; // registration order
    RegistrationObserver m_Observer;
};

} // namespace Editor
} // namespace GameEngine
