#pragma once

#include "UndoRedo/IEditorCommand.h"
#include "Editor/Entities/EntityDisplayName.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Mathematics/Geometry.h"
#include "SplineECS/SplineNetwork.h"

#include <utility>

#include "Components/Spline/SplineNetworkOverrides.h"
#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <memory>
#include <optional>
#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::SplineECS
{
struct SplineNode;
}

namespace GameEngine::Editor
{

/// An undoable edit to one junction rule. The step journals only the record it changes, keyed by
/// its holder, its partner, its rule and where its junction was; undo and redo find the record
/// again through the spline network by that position and write it back through the network,
/// which matches it by that position, so an edit elsewhere in between (an upstream knot drag
/// that re-keyed every arc length) disturbs no other record. A record whose junction is gone
/// when the step replays keeps its journaled keys, and the network holds and reports it.
class SplineNetworkRecordCommand final : public IEditorCommand
{
  private:
    // Only the factories below construct a step: each checks that its record exists.
    struct ConstructionKey
    {
        explicit ConstructionKey() = default;
    };

  public:
    SplineNetworkRecordCommand(ConstructionKey, ECS::World& world, ECS::EntityHandle holder, std::string name);

    /// "Join <road> at Grade": an AtGrade rule on `node`. A node that already has a rule changes
    /// that record where it sits; a node with none gets a new record on `holder`, one of the
    /// node's two members. Null when the node is joined at grade already or `holder` is not
    /// one of its members.
    static std::unique_ptr<SplineNetworkRecordCommand> JoinAtGrade(ECS::World& world, const SplineECS::SplineNode& node,
                                                                   ECS::EntityHandle holder);

    /// "Clear Junction Override": removes the record in `slot` of `holder`, on a junction or
    /// held. Null when the slot is empty.
    static std::unique_ptr<SplineNetworkRecordCommand> Clear(ECS::World& world, ECS::EntityHandle holder, uint32 slot);

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "SplineNetworkRecordCommand"; }

    void Do() override;
    void Undo() override;

  private:
    // Replaces the record `from` (absent: a free slot) with `to` (absent: an empty slot).
    void Apply(const std::optional<Components::SplineNetworkOverride>& from,
               const std::optional<Components::SplineNetworkOverride>& to);
    // The slot holding the record this step journals as `expected`: the one record of that
    // partner and rule whose junction the network places within reach of m_Position, else the
    // slot this step last wrote when the network has not derived since.
    std::optional<uint32> FindSlot(const Components::SplineNetworkOverride& expected) const;
    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Holder{};
    std::string m_Name;
    std::optional<Components::SplineNetworkOverride> m_Before;
    std::optional<Components::SplineNetworkOverride> m_After;
    // Where the record's junction is, in world space: the key undo and redo find it by.
    Mathematics::Vector3 m_Position{};
    std::optional<uint32> m_LastSlot;
    Components::SplineNetworkOverride m_LastWritten{};
};

namespace
{

using Components::SplineNetworkOverride;
using Components::SplineNetworkOverrides;
using Components::SplineNodeRule;
using Mathematics::GroundDistance;
using Mathematics::Vector3;
using SplineECS::SplineNetwork;
using SplineECS::SplineNode;

// Where the network last placed the record in `slot` of `holder`: the junction it rides on, or
// the place it is held. Empty when the network has not derived it.
std::optional<Vector3> RecordPosition(const SplineNetwork& network, const ECS::World& world, ECS::EntityHandle holder,
                                      uint32 slot)
{
    for (const SplineNode& node : network.GetNodes(world))
        if (node.Rule != SplineNodeRule::None && node.RuleHolder == holder && node.RuleSlot == slot)
            return node.Position;
    for (const SplineECS::SplineHeldRecord& held : network.GetHeldRecords(world))
        if (held.Holder == holder && held.Slot == slot)
            return held.LastPosition;
    return std::nullopt;
}

SplineNetworkOverride KeyedTo(const SplineNode& node, ECS::EntityHandle holder, SplineNetworkOverride record)
{
    const uint32 self = node.Members[0] == holder ? 0 : 1;
    record.ArcLengthMetres = node.ArcLengthMetres[self];
    record.OtherArcLengthMetres = node.ArcLengthMetres[1 - self];
    return record;
}

ECS::EntityHandle OtherMember(const SplineNode& node, ECS::EntityHandle member)
{
    return node.Members[0] == member ? node.Members[1] : node.Members[0];
}

} // namespace

inline SplineNetworkRecordCommand::SplineNetworkRecordCommand(ConstructionKey, ECS::World& world, ECS::EntityHandle holder,
                                                       std::string name)
    : m_World(&world)
    , m_Holder(holder)
    , m_Name(std::move(name))
{
}

inline std::unique_ptr<SplineNetworkRecordCommand> SplineNetworkRecordCommand::JoinAtGrade(ECS::World& world,
                                                                                    const SplineNode& node,
                                                                                    ECS::EntityHandle holder)
{
    if (node.Rule == SplineNodeRule::AtGrade || (node.Members[0] != holder && node.Members[1] != holder))
        return nullptr;
    const std::string name = "Join " + EntityDisplayName(world, holder) + " at Grade";
    const ECS::EntityHandle ruleHolder = node.Rule != SplineNodeRule::None ? node.RuleHolder : holder;
    auto command = std::make_unique<SplineNetworkRecordCommand>(ConstructionKey{}, world, ruleHolder, name);
    command->m_Position = node.Position;
    if (node.Rule != SplineNodeRule::None)
    {
        const auto* overrides = world.GetComponent<SplineNetworkOverrides>(ruleHolder);
        if (!overrides || node.RuleSlot >= Components::kMaxSplineNetworkOverrides)
            return nullptr;
        command->m_Before = overrides->Records[node.RuleSlot];
        command->m_LastSlot = node.RuleSlot;
        command->m_LastWritten = *command->m_Before;
    }
    SplineNetworkOverride after{};
    after.Other = OtherMember(node, ruleHolder);
    after.Rule = SplineNodeRule::AtGrade;
    command->m_After = KeyedTo(node, ruleHolder, after);
    return command;
}

inline std::unique_ptr<SplineNetworkRecordCommand> SplineNetworkRecordCommand::Clear(ECS::World& world, ECS::EntityHandle holder,
                                                                              uint32 slot)
{
    const auto* overrides = world.GetComponent<SplineNetworkOverrides>(holder);
    if (!overrides || slot >= Components::kMaxSplineNetworkOverrides ||
        overrides->Records[slot].Rule == SplineNodeRule::None)
        return nullptr;
    auto command =
        std::make_unique<SplineNetworkRecordCommand>(ConstructionKey{}, world, holder, "Clear Junction Override");
    if (const SplineNetwork* network = SplineNetwork::TryGet())
        if (const std::optional<Vector3> position = RecordPosition(*network, world, holder, slot))
            command->m_Position = *position;
    command->m_Before = overrides->Records[slot];
    command->m_LastSlot = slot;
    command->m_LastWritten = *command->m_Before;
    return command;
}

inline void SplineNetworkRecordCommand::Do()
{
    Apply(m_Before, m_After);
}

inline void SplineNetworkRecordCommand::Undo()
{
    Apply(m_After, m_Before);
}

inline void SplineNetworkRecordCommand::Apply(const std::optional<SplineNetworkOverride>& from,
                                       const std::optional<SplineNetworkOverride>& to)
{
    if (!m_World || !m_World->IsValid(m_Holder))
        return;
    SplineNetworkOverrides overrides{};
    if (const auto* existing = m_World->GetComponent<SplineNetworkOverrides>(m_Holder))
        overrides = *existing;
    std::optional<uint32> slot;
    if (from)
    {
        slot = FindSlot(*from);
        if (!slot)
        {
            Logger::Log::Warning("{}: the junction record is no longer on {}; nothing changed", m_Name,
                           EntityDisplayName(*m_World, m_Holder));
            return;
        }
    }
    else
    {
        for (uint32 i = 0; i < Components::kMaxSplineNetworkOverrides && !slot; ++i)
            if (overrides.Records[i].Rule == SplineNodeRule::None)
                slot = i;
        if (!slot)
        {
            Logger::Log::Warning("{}: {} already holds {} junction rules; clear one first", m_Name,
                           EntityDisplayName(*m_World, m_Holder), Components::kMaxSplineNetworkOverrides);
            return;
        }
    }
    m_LastSlot = slot;
    m_LastWritten = to ? *to : SplineNetworkOverride{};
    // Through the network, so the next derivation finds the record by where its junction was and
    // re-keys it there, or holds it when the junction is gone.
    if (SplineNetwork* network = SplineNetwork::TryGet())
    {
        network->SetRecord(*m_World, m_Holder, *slot, m_LastWritten, m_Position);
        return;
    }
    overrides.Records[*slot] = m_LastWritten;
    m_World->AddComponentImmediate(m_Holder, overrides);
}

inline std::optional<uint32> SplineNetworkRecordCommand::FindSlot(const SplineNetworkOverride& expected) const
{
    const auto* overrides = m_World->GetComponent<SplineNetworkOverrides>(m_Holder);
    if (!overrides)
        return std::nullopt;
    std::optional<uint32> found;
    uint32 count = 0;
    if (const SplineNetwork* network = SplineNetwork::TryGet())
    {
        for (uint32 slot = 0; slot < Components::kMaxSplineNetworkOverrides; ++slot)
        {
            const SplineNetworkOverride& record = overrides->Records[slot];
            if (record.Other != expected.Other || record.Rule != expected.Rule)
                continue;
            const std::optional<Vector3> position = RecordPosition(*network, *m_World, m_Holder, slot);
            if (position && GroundDistance(*position, m_Position) <= SplineECS::kNodeMatchMetres)
            {
                found = slot;
                ++count;
            }
        }
    }
    if (count == 1)
        return found;
    if (m_LastSlot && overrides->Records[*m_LastSlot] == m_LastWritten)
        return m_LastSlot;
    return std::nullopt;
}

} // namespace GameEngine::Editor
