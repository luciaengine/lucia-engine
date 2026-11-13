#pragma once

#include "ECS/Entity.h"
#include "Types/Types.h"
#include "UI/UIElement.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine::EditorUI
{

/// The entities one owner lists, in the caller's order. A row has a glyph and a title in the
/// inspector's label column, and a toggle, a note and a mode in its value column. The caller owns the
/// data and the undo steps: each gesture calls one handler, and the caller builds the list again.
/// Layouts: EntityMemberList.uxml, EntityMemberRow.uxml.
class EntityMemberList final : public UIElement
{
  public:
    /// One mode that a row offers. A row offers one mode, shown as a label, or two, shown as
    /// segments. A `Warning` mode takes something away: its filled segment shows the warning color.
    struct Mode
    {
        uint8 Value = 0;
        std::string Label;
        std::string Tooltip;
        bool Warning = false;
    };

    /// One entity of the list. `On` is the toggle (the entity is in the owner's result); only a `Member`'s
    /// mode is filled and can switch; `Modes` empty shows no mode. A row that is not `Live` is a deleted
    /// member: it shows `Words::DeletedTitle`, says that undo restores it, and cannot be selected.
    struct Row
    {
        ECS::EntityHandle Entity{};
        bool On = false;
        bool Member = false;
        std::string Title;
        std::vector<std::string> GlyphClasses;
        std::string GlyphTooltip;
        std::string Note;
        std::vector<Mode> Modes;
        uint8 CurrentMode = 0;
        bool Live = true;
    };

    /// The owner's words. `MemberTooltip` and `CandidateTooltip` are the toggle tooltips of a member and
    /// of any other row. An empty `Heading` shows no heading and no plate: the list is the whole of a
    /// section that names it.
    struct Words
    {
        std::string Heading;
        std::string HeadingTooltip;
        std::string Empty;
        std::string DeletedTitle;
        std::string MemberTooltip;
        std::string CandidateTooltip;
    };

    /// The rows in their order. A non-empty `AddRefusal` turns off the toggle of every row that is not a
    /// member and is its tooltip (the owner holds no more members). While not `Editable`, no toggle and
    /// no mode can change.
    struct Content
    {
        Words Text;
        std::vector<Row> Rows;
        std::string AddRefusal;
        bool Editable = true;
    };

    /// Called with the row's entity. `SetOn` gets the toggle's new value. `Select` is called on a click on
    /// a live row's title.
    struct Handlers
    {
        std::function<void(ECS::EntityHandle, bool)> SetOn;
        std::function<void(ECS::EntityHandle, uint8)> SetMode;
        std::function<void(ECS::EntityHandle)> Select;
    };

    EntityMemberList(const Content& content, const Handlers& handlers);
};

} // namespace GameEngine::EditorUI
