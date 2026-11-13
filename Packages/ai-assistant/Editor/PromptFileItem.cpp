#include "PromptFileItem.h"

#include "PromptFileThumbnail.h"

#include "Logger/Logger.h"
#include "Types/StringId.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <memory>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "ai-assistant:Editor/UI/controls/PromptFileItem.uxml";

// Finds a template element by its id and gives it the item's own id.
template <typename T>
T* TakeElement(UIElement& item, const char* id, size_t index)
{
    T* element = dynamic_cast<T*>(item.FindById(HashStringId(id)));
    if (element)
        element->SetId(std::string(id) + ":" + std::to_string(index));
    return element;
}
} // namespace

PromptFileItem::PromptFileItem(size_t index, std::function<void()> onRemove)
    : m_Index(index)
    , m_OnRemove(std::move(onRemove))
{
    SetFocusable(false);
}

bool PromptFileItem::Build(UIManager& ui)
{
    if (!ui.InstantiateLayoutChildrenFromAssetPath(this, kLayoutAssetPath))
    {
        Logger::Log::Error("AI Assistant: {} did not instantiate; check that the package's assets are staged.",
                           kLayoutAssetPath);
        return false;
    }
    m_Label = TakeElement<Label>(*this, "PromptFileItemLabel", m_Index);
    m_Remove = TakeElement<Button>(*this, "PromptFileItemRemove", m_Index);
    if (!m_Label || !m_Remove)
    {
        Logger::Log::Error("AI Assistant: {} is missing an item element.", kLayoutAssetPath);
        return false;
    }
    m_Remove->SetOnClick([this](UIEvent&) { m_OnRemove(); });
    return true;
}

void PromptFileItem::Show(const std::filesystem::path& path, const std::optional<PromptFile>& file)
{
    if (!m_Label)
        return;
    const std::string name = PathUtf8(path.filename());
    if (file && file->Kind == PromptFileKind::Image)
        InsertChild(0, std::make_unique<PromptFileThumbnail>(path));
    else
        AddClass("prompt-panel-file-item-text");
    if (!file)
        AddClass("prompt-panel-file-item-loading");
    m_Label->SetText(name);
    m_Label->SetTooltip(file ? PathUtf8(path) : "Reading " + PathUtf8(path) + "…");
    m_Remove->SetTooltip("Remove " + name);
}

} // namespace GameEngine
