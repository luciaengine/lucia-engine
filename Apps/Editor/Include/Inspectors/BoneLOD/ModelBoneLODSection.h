#pragma once

#include "Assets/BoneLOD/BoneLODAssetSettings.h"
#include "UI/UIElement.h"

#include <filesystem>
#include <functional>

namespace GameEngine
{
class ModelAsset;
class Label;
class TreeView;

namespace Editor
{
class BoneLODTreeProvider;
class UndoRedoService;

/// Model import settings for the reduced skeletal tier and its required bones.
class ModelBoneLODSection final : public UIElement
{
public:
    ModelBoneLODSection(const ModelAsset& model, UndoRedoService* undo, std::function<void()> requestRefresh);
    ~ModelBoneLODSection() override;

private:
    void BindRow(uint64 id, UIElement* row);
    void SetMandatory(uint64 id, bool mandatory);
    void SetBudget(int budget);
    void ClearMandatory();
    void ResetSettings();
    void Save();

    std::filesystem::path m_Path;
    UndoRedoService* m_Undo = nullptr;
    BoneLODAssetSettings m_Settings;
    std::function<void()> m_RequestRefresh;
    UniquePtr<BoneLODTreeProvider> m_Provider;
    TreeView* m_Tree = nullptr;
    Label* m_Status = nullptr;
};

} // namespace Editor
} // namespace GameEngine
