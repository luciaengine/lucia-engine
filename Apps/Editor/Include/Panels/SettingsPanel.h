#pragma once

#include "UI/Controls/DockPanel.h"
#include "UI/Controls/TreeView.h"
#include "UI/EditorSearchBars.h"
#include "ColorPicker/ColorPickerScope.h"
#include "InspectorRegistry.h"
#include "UI/Interaction/Selection.h"
#include <memory>
#include <filesystem>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <functional>

namespace GameEngine {

struct EditorContext;
struct Stylesheet;
using StylesheetHandle = std::shared_ptr<const Stylesheet>;
class Slider;
class Label;
class FloatField;
class UIManager;
class TreeView;
class TextField;
class Toggle;
class Checkbox;
class UIElement;
class ScrollView;

namespace Editor
{
struct SettingsCategoryDescriptor;
struct SettingsFieldDescriptor;
}

namespace Rendering
{
class IDevice;
}

// Settings category identifiers (start from 1, 0 is reserved as invalid TreeId)
enum class SettingsCategory : uint64_t {
    ProjectSettings = 1,
    UserSettings = 2,
    // Project Settings subcategories
    Camera = 101,
    VersionControl = 104,  // Parent category for VCS settings
    Rendering = 108,       // Project rendering settings (pipeline, quality, etc.)
    Tags = 109,            // Project tags (name + color for asset labeling)
    HDROutput = 120,       // Project HDR output settings and display diagnostics
    // User Settings subcategories
    UI = 201,
    Shortcuts = 202,
    // UI sub-categories
    UIColors = 300,
    UIAppearance = 301,
    UIFontSizes = 302,
    UITrees = 303,
    UIAssets = 304,
    UIInspector = 305,
    UIHiDpi = 306,
    };

// Per-provider VCS tabs are dynamic children of VersionControl: TreeId =
// kVcsProviderCategoryBase + index into the EditorVcsProviderRegistry snapshot
// (detection order). They flow through SettingsCategory casts but never match
// a named enum case.
inline constexpr uint64_t kVcsProviderCategoryBase = 5000;
inline constexpr uint64_t kVcsProviderCategoryMaxCount = 64;

inline bool IsVcsProviderCategory(SettingsCategory category)
{
    const auto value = static_cast<uint64_t>(category);
    return value >= kVcsProviderCategoryBase &&
           value < kVcsProviderCategoryBase + kVcsProviderCategoryMaxCount;
}

// Categories registered through Editor::EditorSettingsRegistry are dynamic
// children of a settings root: TreeId = kRegistrySettingsCategoryBase + index
// into the registry snapshot (registration order; replace-forward keeps
// indices stable). They flow through SettingsCategory casts but never match a
// named enum case.
inline constexpr uint64_t kRegistrySettingsCategoryBase = 10000;
inline constexpr uint64_t kRegistrySettingsCategoryMaxCount = 1024;

inline bool IsRegistrySettingsCategory(SettingsCategory category)
{
    const auto value = static_cast<uint64_t>(category);
    return value >= kRegistrySettingsCategoryBase &&
           value < kRegistrySettingsCategoryBase + kRegistrySettingsCategoryMaxCount;
}

// Data provider for the settings tree
class SettingsTreeDataProvider : public TreeChangeTrackingProvider {
public:
    SettingsTreeDataProvider();
    
    int GetRootCount() const override;
    TreeId GetRootId(int index) const override;
    int GetChildCount(TreeId parent) const override;
    TreeId GetChildId(TreeId parent, int index) const override;
    const char* GetLabel(TreeId id) const override;
    bool IsExpandable(TreeId id) const override;

    /** Returns set of tree ids that should be expanded so every node whose label contains \p query (case-insensitive) is visible. */
    std::unordered_set<TreeId> GetAncestorIdsToExpandForSearch(const std::string& query) const;

    /** Returns set of tree ids that should be expanded so every node in \p nodeIds is visible (ancestors of each node). */
    std::unordered_set<TreeId> GetAncestorIdsToExpandForNodes(const std::unordered_set<TreeId>& nodeIds) const;

    /**
     * Appends categories for tree nodes whose titles match \p lowerQuery (already lowercased),
     * in DFS order from roots. Skips ids already present in \p seen (typically curated search hits).
     */
    void AppendCategoriesMatchingTreeLabels(const std::string& lowerQuery,
                                            std::vector<SettingsCategory>& outCategories,
                                            std::unordered_set<SettingsCategory>& seen) const;

    /**
     * Row class of each registered category, indexed by its id minus
     * kRegistrySettingsCategoryBase; empty where the category declares none.
     * Filled by RefreshRegistryNodes, which runs before a registry id can
     * enter the tree's flat list and on each registration, so a bound
     * registry row always has its class here. Takes no registry snapshot.
     */
    const std::vector<std::string>& GetRegistryRowClasses() const { return m_RegistryRowClasses; }

private:
    struct TreeNode {
        std::string Label;
        std::vector<TreeId> Children;
        bool Expandable = false;
    };

    // Rebuilds the dynamic per-provider VCS tab nodes from the
    // EditorVcsProviderRegistry so tabs registered after construction (package
    // modules load at project open) appear on the next tree query.
    void RefreshVcsProviderNodes() const;

    // Same for categories registered through Editor::EditorSettingsRegistry:
    // upserts one node per snapshot entry and re-merges each root's children.
    void RefreshRegistryNodes() const;

    mutable std::unordered_map<TreeId, TreeNode> m_Nodes;
    mutable std::vector<std::string> m_RegistryRowClasses;
    std::vector<TreeId> m_Roots;
    // Hand-built children per root, captured at construction; registered
    // categories merge into these on every refresh.
    mutable std::unordered_map<TreeId, std::vector<TreeId>> m_LegacyRootChildren;
};

enum class LayoutMode {
    Vertical,    // Tree on top, content below
    Horizontal   // Tree on left, content on right
};

// Searchable setting item
struct SearchableSettingItem {
    std::string Label;           // Display name for search matching
    std::string Keywords;        // Additional keywords for search
    SettingsCategory Category;   // Which category this belongs to
    std::function<void(UIElement*)> CreateUI; // Function to create the UI element
};

class SettingsPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "settings-icon"; }

    SettingsPanel();
    ~SettingsPanel() override;

    void SetContext(EditorContext* context);
    void SetOpenColorPickerWindow(OpenColorPickerWindowFn fn) { m_OpenColorPickerWindow = std::move(fn); }
    void SetOnHdrOutputSettingsChanged(std::function<void()> cb) { m_OnHdrOutputSettingsChanged = std::move(cb); }
    // Invoked when a preview-thumbnail setting (e.g. IBL on/off, resolution) changes
    // and already-rendered thumbnails were invalidated, so panels that display model
    // thumbnails (Hierarchy icons) can re-fetch them.
    void SetOnPreviewThumbnailsChanged(std::function<void()> cb) { m_OnPreviewThumbnailsChanged = std::move(cb); }

    // Device used to read live HDR display capabilities; gates the HDR Mode dropdown to the
    // colorspaces the active display actually supports. Null = no device, fall back to all modes.
    // Invariant: this is the main window's device, which outlives the SettingsPanel, so the raw
    // pointer never dangles. If that ever changes, clear it (SetDevice(nullptr)) on device teardown.
    void SetDevice(Rendering::IDevice* device) { m_HdrDevice = device; }

    // Set the UIManager to configure (required for AA strength slider)
    void SetUIManager(UIManager* uiManager);

    void SetOnHierarchyTreeRowHeightChanged(std::function<void(float)> cb)
    {
        m_OnHierarchyTreeRowHeightChanged = std::move(cb);
    }
    void SetOnAssetsTreeRowHeightChanged(std::function<void(float)> cb) { m_OnAssetsTreeRowHeightChanged = std::move(cb); }
    void SetOnGetHierarchyTreeRowHeight(std::function<float()> cb) { m_OnGetHierarchyTreeRowHeight = std::move(cb); }
    void SetOnGetAssetsTreeRowHeight(std::function<float()> cb) { m_OnGetAssetsTreeRowHeight = std::move(cb); }
    void SetOnHierarchyTreeChildIndentChanged(std::function<void(float)> cb)
    {
        m_OnHierarchyTreeChildIndentChanged = std::move(cb);
    }
    void SetOnAssetsTreeChildIndentChanged(std::function<void(float)> cb)
    {
        m_OnAssetsTreeChildIndentChanged = std::move(cb);
    }
    void SetOnHierarchyTreeIconSizeChanged(std::function<void(float)> cb)
    {
        m_OnHierarchyTreeIconSizeChanged = std::move(cb);
    }
    void SetOnAssetsTreeIconSizeChanged(std::function<void(float)> cb) { m_OnAssetsTreeIconSizeChanged = std::move(cb); }
    // Notify other panels when the base UI font size changes (UI Text Scale slider).
    void SetOnBaseFontSizeChanged(std::function<void(float)> cb) { m_OnBaseFontSizeChanged = std::move(cb); }
    /// Fan out runtime font preference CSS to every editor window (main + floating).
    void SetOnEditorFontPreferencesChanged(std::function<void()> cb) { m_OnEditorFontPreferencesChanged = std::move(cb); }
    /// Re-apply HiDPI / content-scale preferences to every editor window platform adapter.
    void SetOnHiDpiPlatformSettingsChanged(std::function<void()> cb) { m_OnHiDpiPlatformSettingsChanged = std::move(cb); }
    void SetOnGridIconSizeChanged(std::function<void(float)> cb) { m_OnGridIconSizeChanged = std::move(cb); }
    void SetOnGridIconSizeFinalized(std::function<void(float)> cb) { m_OnGridIconSizeFinalized = std::move(cb); }
    void SetOnSmartFoldersAtTopChanged(std::function<void(bool)> cb) { m_OnSmartFoldersAtTopChanged = std::move(cb); }
    void SetOnSmartFoldersExpandedOnStartupChanged(std::function<void(bool)> cb) { m_OnSmartFoldersExpandedOnStartupChanged = std::move(cb); }
    void SetOnAssetsFoldersFirstChanged(std::function<void(bool)> cb) { m_OnAssetsFoldersFirstChanged = std::move(cb); }
    void SetOnAssetsExpandFoldersOnLoadChanged(std::function<void(bool)> cb) { m_OnAssetsExpandFoldersOnLoadChanged = std::move(cb); }
    void SetOnAssetsExtraBottomViewToolbarChanged(std::function<void(bool)> cb) { m_OnAssetsExtraBottomViewToolbarChanged = std::move(cb); }
    void SetOnAssetsSingleViewToggleIconChanged(std::function<void(bool)> cb) { m_OnAssetsSingleViewToggleIconChanged = std::move(cb); }
    void SetOnAssetsBottomToolbarZoomSliderVisibleChanged(std::function<void(bool)> cb)
    {
        m_OnAssetsBottomToolbarZoomSliderVisibleChanged = std::move(cb);
    }
    // Fired after a project render setting that needs the shared apply path is
    // persisted. Carries no value: the re-apply reads one SettingsStore
    // snapshot and fans it out to every window.
    void SetOnProjectRenderSettingsChanged(std::function<void()> cb)
    {
        m_OnProjectRenderSettingsChanged = std::move(cb);
    }
    void SetOnInspectorCollapseArrowVisibilityChanged(std::function<void(bool)> cb) { m_OnInspectorCollapseArrowVisibilityChanged = std::move(cb); }
    void SetOnInspectorComponentIconsVisibilityChanged(std::function<void(bool)> cb) { m_OnInspectorComponentIconsVisibilityChanged = std::move(cb); }
    void SetOnInspectorFilledSectionsChanged(std::function<void(bool)> cb) { m_OnInspectorFilledSectionsChanged = std::move(cb); }
    void SetOnInspectorInfoCardsChanged(std::function<void(bool)> cb) { m_OnInspectorInfoCardsChanged = std::move(cb); }
    void SetOnInspectorToggleAlignChanged(std::function<void(const std::string&)> cb);
    void SetOnInspectorSoloSectionsChanged(std::function<void(bool)> cb) { m_OnInspectorSoloSectionsChanged = std::move(cb); }
    void SetOnInspectorSoloKeepTransformChanged(std::function<void(bool)> cb) { m_OnInspectorSoloKeepTransformChanged = std::move(cb); }
    void SetOnTruncationThresholdChanged(std::function<void(float)> cb) { m_OnTruncationThresholdChanged = std::move(cb); }
    void SetOnTruncationEnabledChanged(std::function<void(bool)> cb) { m_OnTruncationEnabledChanged = std::move(cb); }
    void SetOnAssetsListColumnsChanged(std::function<void()> cb) { m_OnAssetsListColumnsChanged = std::move(cb); }
    void SetPingAsset(std::function<void(const std::filesystem::path&)> cb) { m_PingAsset = std::move(cb); }

    void SetHierarchyTreeIconSizeValue(float px);
    void SetAssetsTreeIconSizeValue(float px);
    void SetHierarchyTreeIconSizeValueAndPersist(float px);
    void SetAssetsTreeIconSizeValueAndPersist(float px);
    void SetGridIconSizeValue(float px);
    void SetInspectorToggleAlignValue(const std::string& value);

    // Get global search bar visibility state
    static bool GetSearchBarsVisible() { return EditorSearchBars::GetVisible(); }


    /** Apply saved "Colored Hierarchy Icons" toggle (adds/removes root class). */
    static void ApplySavedHierarchyIconsColoredStyle(UIManager* ui);

    /** Apply saved "3D Model Thumbnails Always Colored" toggle (adds/removes `hierarchy-model-thumbs-colored` root class). */
    static void ApplySavedHierarchyModelThumbsAlwaysColoredStyle(UIManager* ui);



    /** Apply saved "Big Number Spacing" to every FloatField (process-wide; the Settings page builds lazily). */
    static void ApplySavedInspectorBigNumberSpacing();






    /** Apply saved node graph node-header alignment to any mounted node graph canvas. */
    static void ApplySavedNodeGraphHeaderAlignmentStyle(UIManager* ui);

    // Register/unregister search bars for real-time updates
    static void RegisterSearchBar(UIElement* searchBar, UIElement* /*icon*/) { EditorSearchBars::Register(searchBar); }
    static void UnregisterSearchBar(UIElement* searchBar) { EditorSearchBars::Unregister(searchBar); }

    /// Request that the panel show a specific category (e.g. from Add Tag modal "All Tags…").
    void RequestShowCategory(SettingsCategory category);
    /// Request a category registered through EditorSettingsRegistry by stable id.
    void RequestShowRegistryCategory(std::string_view categoryId);

    /// Search metadata shared with the global command palette. The vector is populated
    /// when the settings tree is built and remains owned by this panel.
    const std::vector<SearchableSettingItem>& GetSearchableItems() const { return m_SearchableItems; }

private:
    void BuildUI();
    void RebuildLayout(LayoutMode newMode);
    void CreateTreeView(UIElement* parent);
    void CreateContentPane(UIElement* parent);
    void CreateSearchBar(UIElement* parent);
    void OnPostLayout() override;
    void BindFromAssetsDeferred();
    void OnCategorySelected(TreeId id);
    void ShowCategoryContent(SettingsCategory category);
    void CreateUISettingsContent();
    void CreateUIColorsContent();
    void CreateUIAppearanceContent();
    void CreateUIHiDpiContent();
    void CreateUIFontSizesContent();
    void CreateUITreesContent();
    void CreateUIAssetsContent();
    void CreateUIInspectorContent();
    void CreateUserSettingsContent();
    void CreateCameraSettingsContent();
    void SyncCameraSettingsControls();
    void CreateRenderingSettingsContent();
    void NotifyProjectRenderSettingsChanged();
    void CreateHDROutputSettingsContent();
public:
    // Called by other systems (e.g. RenderPipelineInspector) when the active
    // render pipeline is changed elsewhere, so any open Settings panel can
    // update its dropdown without requiring a category re-open.
    static void NotifyActivePipelineChanged(const std::string& assetRelativePath);
private:
    void CreateTagsSettingsContent();
    void CreateShortcutsSettingsContent();
    void CreateProjectSettingsContent();
    void CreateVersionControlContent();
    // Per-provider VCS tabs are dynamic: content comes from
    // EditorVcsProviderRegistry descriptors (BuildSettingsContent).
    void CreateVcsProviderContent(const std::string& providerTypeId);
    void ClearContentPane();
    void CreateTreeScalingSettingsForTarget(bool forHierarchy);
    void ApplyHierarchyTreeIconSize(float px);
    void ApplyAssetsTreeIconSize(float px);
    void PerformSearch(const std::string& query);
    void RequestApplySearchState();
    void ApplySearchStateNow();
    void ShowSearchResults(const std::string& query);
    void OpenSearchResultInContext(SettingsCategory category,
                                   const std::string& label,
                                   const std::string& query);
    void HighlightSearchResultInContext(const std::string& label,
                                        const std::string& query);
    void ScrollPendingSearchRowIntoView();
    void ClearSearchResults();
    void ClearSearchHighlights();
    void ApplySearchFilterToContent(const std::string& searchText);
    void RegisterSearchableItems();
    void CreateRegistrySettingsContent(const Editor::SettingsCategoryDescriptor& descriptor);
    void CreateRegistryFieldRow(const Editor::SettingsFieldDescriptor& field, const std::string& pageTitle);
    // Records the content elements added since firstElement as one conditional
    // field and applies its condition.
    void TrackConditionalRegistryRows(size_t firstElement, std::function<bool()> isVisible);
    void RefreshConditionalRegistryRows();
    void OpenColorPicker(uint32_t initialArgb,
                         std::function<void(uint32_t)> onApply,
                         std::function<void()> onCancel,
                         std::function<void(uint32_t)> onValueChanging = {});
    
    bool IsParentCategory(SettingsCategory category) const;
    void UpdateTreePaneHeight();
    void ExportUserSettings();
    void ShowImportUserSettingsModal(const std::filesystem::path& importPath);
    int GetVisibleItemCount() const;

    // Tree and layout components
    TreeView* m_TreeView = nullptr;
    UIElement* m_TreePane = nullptr;
    UIElement* m_ContentPane = nullptr;
    UIElement* m_VerticalSplitter = nullptr;
    UIElement* m_SearchBar = nullptr;
    Label* m_ContentHeader = nullptr;
    ScrollView* m_ContentBody = nullptr;
    TextField* m_SearchField = nullptr;
    std::shared_ptr<bool> m_LifetimeToken = std::make_shared<bool>(true);

    // Vertical-mode tree pane height: 0 = auto-size to content; >0 = user override.
    float m_UserTreePaneHeightPx = 0.0f;
    bool m_TreeSplitterDragging = false;
    float m_TreeSplitterDragStartY = 0.0f;
    float m_TreeSplitterDragStartHeight = 0.0f;

    // Layout mode tracking
    LayoutMode m_CurrentLayoutMode = LayoutMode::Vertical;
    float m_LastAspectRatio = 0.0f;
    
    // Data providers
    std::unique_ptr<SettingsTreeDataProvider> m_TreeDataProvider;
    std::unique_ptr<UI::Interaction::SelectionModel> m_TreeSelectionModel;

    // UI elements for settings
    Slider* m_HierarchyTreeChildIndentSlider = nullptr;
    Label* m_HierarchyTreeChildIndentLabel = nullptr;
    Slider* m_HierarchyTreeRowHeightSlider = nullptr;
    Label* m_HierarchyTreeRowHeightLabel = nullptr;
    Slider* m_HierarchyTreeIconSizeSlider = nullptr;
    Label* m_HierarchyTreeIconSizeLabel = nullptr;
    FloatField* m_HierarchyTreeIconSizeField = nullptr;
    Slider* m_AssetsTreeChildIndentSlider = nullptr;
    Label* m_AssetsTreeChildIndentLabel = nullptr;
    Slider* m_AssetsTreeRowHeightSlider = nullptr;
    Label* m_AssetsTreeRowHeightLabel = nullptr;
    Slider* m_AssetsTreeIconSizeSlider = nullptr;
    Label* m_AssetsTreeIconSizeLabel = nullptr;
    FloatField* m_AssetsTreeIconSizeField = nullptr;
    Slider* m_GridIconSizeSlider = nullptr;
    FloatField* m_GridIconSizeField = nullptr;
    Toggle* m_SearchBarsToggle = nullptr;
    Toggle* m_CheckmarkTogglesToggle = nullptr;
    Toggle* m_TabIconsToggle = nullptr;
    TextField* m_FontSmoothingField = nullptr;
    
    // Font size sliders
    Slider* m_FontSizeBaseSlider = nullptr;
    Slider* m_FontSizeTreeSlider = nullptr;
    Slider* m_FontSizeSmallSlider = nullptr;
    Slider* m_FontSizeHeaderSlider = nullptr;
    float m_HierarchyTreeIconSizeValue = 20.0f;
    bool m_HierarchyTreeIconSizeUpdating = false;
    float m_AssetsTreeIconSizeValue = 20.0f;
    bool m_AssetsTreeIconSizeUpdating = false;
    float m_GridIconSizeValue = 80.0f;
    bool m_GridIconSizeUpdating = false;
    bool m_GridIconSizePending = false;
    
    // Closed whenever content-page elements are destroyed: a colour picker the
    // page opened closes with it instead of calling into the destroyed page.
    Editor::ColorPickerScope m_PageColorPickers;
    // Inspector row indent + gap + label width: runtime stylesheet
    Slider* m_InspectorRowIndentSlider = nullptr;
    Slider* m_InspectorRowGapSlider = nullptr;
    Slider* m_InspectorLabelWidthSlider = nullptr;
    void SetInspectorRowLayout(float indent, float gap, float labelWidthPercent);
    // UI scrollbar thumb color (Settings > UI): runtime stylesheet
    uint32_t m_UIScrollbarColor = 0xFF383838;
    StylesheetHandle m_UIScrollbarStylesheet;
    StylesheetHandle m_UISceneToolStylesheet;
    EditorContext* m_EditorContext = nullptr;
    Rendering::IDevice* m_HdrDevice = nullptr;
    OpenColorPickerWindowFn m_OpenColorPickerWindow;
    std::function<void()> m_OnHdrOutputSettingsChanged;
    std::function<void()> m_OnPreviewThumbnailsChanged;
    void ApplyScrollbarColorToUI(); // Update runtime styles for scrollbar thumb color
    void ApplySceneToolActiveColor(const std::string& mode); // accent or gray
    // Sets a colour CSS variable ("#RRGGBB") on the UI root and restyles the tree.
    void SetRootColorVariable(std::string_view name, uint32_t argb);

    // One colour setting row: label (double-click resets to DefaultArgb) and a
    // swatch that opens the editor color picker, optionally followed by a hex field.
    struct ColorSwatchRowConfig
    {
        std::string LabelText;
        std::string Tooltip;
        uint32_t InitialArgb = 0;
        uint32_t DefaultArgb = 0;
        bool ShowHexField = false;
        // persist is false for picker live previews and for the revert on cancel.
        std::function<void(uint32_t argb, bool persist)> Apply;
    };
    void CreateColorSwatchRow(const ColorSwatchRowConfig& config);

    UIManager* m_UIManager = nullptr;

    bool m_BindApplied = false;
    bool m_BindScheduled = false;
    bool m_UIBuilt = false;
    bool m_RebuildScheduled = false;
    bool m_InitialTreeFocusApplied = false;
    bool m_InitialTreeScrollApplied = false;
    uint64_t m_SceneViewCameraSettingsListenerId = 0;
    
    SettingsCategory m_CurrentCategory = SettingsCategory::UIAppearance;
    
    // Track last used child for each parent category
    std::unordered_map<SettingsCategory, SettingsCategory> m_LastUsedChild;
    
    // Track last panel height for responsive updates
    float m_LastPanelHeight = 0.0f;

    std::function<void(float)> m_OnHierarchyTreeRowHeightChanged;
    std::function<void(float)> m_OnAssetsTreeRowHeightChanged;
    std::function<float()> m_OnGetHierarchyTreeRowHeight;
    std::function<float()> m_OnGetAssetsTreeRowHeight;
    std::function<void(float)> m_OnHierarchyTreeChildIndentChanged;
    std::function<void(float)> m_OnAssetsTreeChildIndentChanged;
    std::function<void(float)> m_OnHierarchyTreeIconSizeChanged;
    std::function<void(float)> m_OnAssetsTreeIconSizeChanged;
    std::function<void(float)> m_OnBaseFontSizeChanged;
    std::function<void()> m_OnEditorFontPreferencesChanged;
    std::function<void()> m_OnHiDpiPlatformSettingsChanged;
    std::function<void(float)> m_OnGridIconSizeChanged;
    std::function<void(float)> m_OnGridIconSizeFinalized;
    std::function<void(bool)> m_OnSmartFoldersAtTopChanged;
    std::function<void(bool)> m_OnSmartFoldersExpandedOnStartupChanged;
    Toggle* m_SmartFoldersAtTopToggle = nullptr;
    Toggle* m_SmartFoldersExpandedOnStartupToggle = nullptr;
    std::function<void(bool)> m_OnAssetsFoldersFirstChanged;
    std::function<void(bool)> m_OnAssetsExpandFoldersOnLoadChanged;
    std::function<void(bool)> m_OnAssetsExtraBottomViewToolbarChanged;
    std::function<void(bool)> m_OnAssetsSingleViewToggleIconChanged;
    std::function<void(bool)> m_OnAssetsBottomToolbarZoomSliderVisibleChanged;
    std::function<void()> m_OnProjectRenderSettingsChanged;
    Toggle* m_AssetsFoldersFirstToggle = nullptr;
    Toggle* m_AssetsExpandFoldersOnLoadToggle = nullptr;
    Toggle* m_AssetsExtraBottomViewToolbarToggle = nullptr;
    Toggle* m_AssetsSingleViewToggleIconToggle = nullptr;
    Toggle* m_AssetsBottomToolbarZoomSliderVisibleToggle = nullptr;
    std::function<void(bool)> m_OnInspectorCollapseArrowVisibilityChanged;
    Toggle* m_InspectorCollapseArrowVisibleToggle = nullptr;
    std::function<void(bool)> m_OnInspectorComponentIconsVisibilityChanged;
    Toggle* m_InspectorComponentIconsVisibleToggle = nullptr;
    std::function<void(bool)> m_OnInspectorFilledSectionsChanged;
    Toggle* m_InspectorFilledSectionsToggle = nullptr;
    std::function<void(bool)> m_OnInspectorInfoCardsChanged;
    Toggle* m_InspectorInfoCardsToggle = nullptr;
    Toggle* m_InspectorBigNumberSpacingToggle = nullptr;
    std::function<void(bool)> m_OnInspectorSoloSectionsChanged;
    Toggle* m_InspectorSoloSectionsToggle = nullptr;
    std::function<void(bool)> m_OnInspectorSoloKeepTransformChanged;
    Toggle* m_InspectorSoloKeepTransformToggle = nullptr;
    Toggle* m_GraySlidersToggle = nullptr;
    Toggle* m_CompactComponentHeadersToggle = nullptr;
    std::function<void(const std::string&)> m_OnInspectorToggleAlignChanged;
    Checkbox* m_InspectorToggleAlignLeft = nullptr;
    Checkbox* m_InspectorToggleAlignMiddle = nullptr;
    Checkbox* m_InspectorToggleAlignRight = nullptr;
    std::function<void(float)> m_OnTruncationThresholdChanged;
    std::function<void(bool)> m_OnTruncationEnabledChanged;
    std::function<void()> m_OnAssetsListColumnsChanged;
    std::function<void(const std::filesystem::path&)> m_PingAsset;
    
    // Search functionality (Inspector-style: highlight in tree and content, expand tree to show matches)
    std::vector<SearchableSettingItem> m_SearchableItems;
    std::string m_CurrentSearchQuery;
    bool m_ShowingSearchResults = false;
    bool m_SearchApplyPosted = false;
    std::vector<UIElement*> m_SearchHighlightedElements;
    std::vector<UIElement*> m_SearchFilteredElements;
    /** Categories that contain at least one search match, in tree order. */
    std::vector<SettingsCategory> m_SearchMatchCategories;
    struct PendingSearchContextReveal {
        SettingsCategory Category;
        std::string Label;
        std::string Query;
    };
    std::optional<PendingSearchContextReveal> m_PendingSearchContextReveal;
    // The row a search reveal still has to scroll into view. Centering it needs
    // real layout geometry, which only exists once layout has run, so the scroll
    // waits for OnPostLayout rather than for a guessed number of action drains.
    std::optional<PendingSearchContextReveal> m_PendingSearchScroll;
    struct SearchSegmentReplacement {
        UIElement* parent = nullptr;
        std::unique_ptr<UIElement> OriginalLabel;
        UIElement* segmentContainer = nullptr;
    };
    std::vector<SearchSegmentReplacement> m_SearchSegmentReplacements;

    // The elements one registered field put on the page (section break, row,
    // info card) and the condition that shows them. Re-evaluated after layout
    // and after every toggle or dropdown change on the page, so a row can hide
    // while its page stays open. Cleared with the content.
    struct ConditionalRegistryRows
    {
        std::vector<UIElement*> Elements;
        std::function<bool()> IsVisible;
    };
    std::vector<ConditionalRegistryRows> m_ConditionalRegistryRows;
};

} // namespace GameEngine
