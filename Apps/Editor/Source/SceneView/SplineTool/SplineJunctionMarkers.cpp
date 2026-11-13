#include "SceneView/SplineTool/SplineJunctionMarkers.h"

#include "Components/Name.h"
#include "Core/CpuProfiler.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "Editor/Settings/SplineEditorSettings.h"
#include "Inspectors/Spline/SplineInspector.h"
#include "Inspectors/SplineRecipe/Paving/SplinePavingInspector.h"
#include "SceneView/SceneViewGizmos.h"
#include "SceneView/SplineTool/SplineTool.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineRecipes/Paving/PavingSources.h"
#include "SplineRecipes/Paving/PavingStatus.h"
#include "SplineRecipes/RecipeControllers.h"
#include "Types/Fnv1a.h"
#include "Types/FormatNumber.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <span>
#include <string_view>

namespace GameEngine::Editor::SceneTools
{

using Mathematics::Vector3;
using SplineECS::SplineNode;
using SplineECS::SplineNodeKind;

namespace
{

// The gizmo pass reads no stylesheet: these are the values of the theme's tokens (tokens.css),
// copied, so a change to a token is made here too.
// --ui_color_text_bright: a junction that joins.
const Color kJoinedColor(1.0f, 1.0f, 1.0f, 1.0f);
// --ui_color_warning_line (#B5852C): a junction whose heights differ.
const Color kHeightsDifferColor(181.0f / 255.0f, 133.0f / 255.0f, 44.0f / 255.0f, 1.0f);
// --ui_color_prefab (#2EC4B6), the editor's teal: a junction that just formed, and the snap ring.
const Color kAttachColor(46.0f / 255.0f, 196.0f / 255.0f, 182.0f / 255.0f, 1.0f);
// --ui_color_view_banner_background: the plate under a glyph and the edge under every line, so a
// marker keeps its edge on a light road and on grass.
const Color kBackingColor(0.0f, 0.0f, 0.0f, 0.55f);

// The ring's radius as a multiple of the knot's pick radius, so a marker reads larger than a knot.
constexpr float32 kMarkerRadiusScale = 1.25f;
// The glyph's half-extent as a fraction of the ring's radius.
constexpr float32 kGlyphScale = 0.55f;
// A hovered marker's lines, and the snap ring's, are this much thicker.
constexpr float32 kEmphasisThicknessScale = 1.75f;
// The dark edge is this much wider than the line on it, in pixels: one pixel on each side.
constexpr float32 kBackingExtraThickness = 2.0f;
// A marker within this many ring radii of a live snap point hides under the snap ring.
constexpr float32 kSnapHidesMarkerRadii = 2.0f;
// A height difference shows this many decimals, as the paving inspector's Junctions rows show it.
constexpr int kHeightDecimals = 2;
// How long a junction that just formed stays teal.
constexpr std::chrono::milliseconds kFirstSettleDuration{1500};

float32 MarkerRadius(float32 screenScale)
{
    return SplineKnotPickBase() * kMarkerRadiusScale * screenScale;
}

// The arm of the node's first member: for a T and an entry, the road that ends there.
Vector3 FirstMemberForward(const SplineNode& node)
{
    for (uint32 i = 0; i < node.ArmCount; ++i)
    {
        if (node.Arms[i].Member == node.Members[0])
            return node.Arms[i].Forward;
    }
    return Vector3{0.0f, 0.0f, 1.0f};
}

struct GlyphFrame
{
    Vector3 Center;
    // Along the first member's arm, and across it, in the marker's plane; unit length.
    Vector3 Along;
    Vector3 Across;
    float32 Extent = 0.0f;
};

struct LinePen
{
    Color LineColor;
    float32 Thickness = 1.0f;
    bool ConstantScreen = true;
    bool SmartDistance = false;
};

// One glyph stroke from `a` to `b`, in glyph units around the frame's center.
void DrawGlyphLine(GizmoRenderContext& context, const GlyphFrame& frame, const LinePen& pen, const Vector3& a,
                   const Vector3& b)
{
    DrawThickLine(context, frame.Center + a * frame.Extent, frame.Center + b * frame.Extent, pen.LineColor,
                  pen.Thickness, pen.ConstantScreen, pen.SmartDistance);
}

void DrawGlyph(GizmoRenderContext& context, SplineNodeKind kind, const GlyphFrame& frame, const LinePen& pen)
{
    const Vector3& u = frame.Along;
    const Vector3& v = frame.Across;
    switch (kind)
    {
        case SplineNodeKind::Crossing:
            DrawGlyphLine(context, frame, pen, u + v, (u + v) * -1.0f);
            DrawGlyphLine(context, frame, pen, u - v, v - u);
            break;
        case SplineNodeKind::Attachment:
            // The bar lies along the body, the stem along the road that ends on it.
            DrawGlyphLine(context, frame, pen, v * -1.0f, v);
            DrawGlyphLine(context, frame, pen, Vector3{}, u * 1.6f);
            break;
        case SplineNodeKind::Entry:
            // A bar for the outline, and an arrow from the road into it.
            DrawGlyphLine(context, frame, pen, v * -1.0f - u * 0.5f, v - u * 0.5f);
            DrawGlyphLine(context, frame, pen, u * 1.2f, u * -0.2f);
            DrawGlyphLine(context, frame, pen, u * -0.2f, u * 0.25f + v * 0.45f);
            DrawGlyphLine(context, frame, pen, u * -0.2f, u * 0.25f - v * 0.45f);
            break;
        case SplineNodeKind::Merge:
            DrawGlyphLine(context, frame, pen, u * -1.0f, u);
            DrawGlyphLine(context, frame, pen, v * -0.5f, v * 0.5f);
            break;
    }
}

// The plate, then the dark edge under the ring and the glyph, then the ring and the glyph.
void DrawMarker(GizmoRenderContext& context, SplineNodeKind kind, const GlyphFrame& frame, const Vector3& normal,
                float32 radius, const LinePen& pen)
{
    context.DrawSolidDisc(frame.Center, normal, radius, kBackingColor);
    const LinePen backing{kBackingColor, pen.Thickness + kBackingExtraThickness, pen.ConstantScreen,
                          pen.SmartDistance};
    DrawThickWireCircle(context, frame.Center, normal, radius, backing.LineColor, backing.Thickness,
                        backing.ConstantScreen, backing.SmartDistance);
    DrawGlyph(context, kind, frame, backing);
    DrawThickWireCircle(context, frame.Center, normal, radius, pen.LineColor, pen.Thickness, pen.ConstantScreen,
                        pen.SmartDistance);
    DrawGlyph(context, kind, frame, pen);
}

// The record of `node` in the paving status of the first member that reports it; null where no
// paving reports the node (two roads that cross).
const SplineRecipes::PavingJunctionStatus* FindPavingJunction(const SplineNode& node)
{
    const SplineRecipes::RecipeControllers* controllers = RunningRecipeControllers();
    if (!controllers)
        return nullptr;
    for (const ECS::EntityHandle member : node.Members)
    {
        const SplineRecipes::PavingStatus* status = controllers->PavingStatusOf(member);
        if (!status)
            continue;
        for (const SplineRecipes::PavingJunctionStatus& junction : status->Junctions)
        {
            if (junction.NodeId == node.Id)
                return &junction;
        }
    }
    return nullptr;
}

// The state as a paving that reaches the node reports it, in the inspector's words; the
// network's own words where no paving reports the node.
std::string NodeState(const ECS::World& world, const SplineNode& node)
{
    if (const SplineRecipes::PavingJunctionStatus* junction = FindPavingJunction(node))
        return PavingJunctionStateWords(world, *junction);
    using Components::SplineNodeRule;
    if (node.Rule == SplineNodeRule::SplitLevel)
        return "not joined";
    if (node.Rule == SplineNodeRule::Closed)
        return "closed";
    if (!node.IsSplitLevel())
        return "joined";
    // The difference is the second member (the body or the area) minus the first (the road).
    const float32 difference = node.HeightDifferenceMetres;
    const std::string meters = FormatMeters(std::fabs(difference), kHeightDecimals);
    if (node.Kind == SplineNodeKind::Attachment || node.Kind == SplineNodeKind::Entry)
        return meters + (difference > 0.0f ? " below" : " above");
    return "heights differ by " + meters;
}

// The Name component's text, or empty: a name the hover text can show without allocating.
std::string_view NameView(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* name = world.GetComponent<Components::Name>(entity);
    return name ? name->View() : std::string_view{};
}

// A hash of every value Tooltip reads for `node`, read without allocating: equal keys give the
// same text.
uint64 HoverTextKey(const ECS::World& world, const SplineNode& node)
{
    using Hashing::Fnv1a64;
    using Hashing::Fnv1a64Value;
    uint64 key = Fnv1a64Value(Hashing::kFnv1a64OffsetBasis, node.Id);
    key = Fnv1a64Value(key, node.Kind);
    key = Fnv1a64Value(key, node.Rule);
    key = Fnv1a64Value(key, node.HeightDifferenceMetres);
    for (const ECS::EntityHandle member : node.Members)
        key = Fnv1a64(NameView(world, member), Fnv1a64Value(key, member));
    const SplineRecipes::PavingJunctionStatus* junction = FindPavingJunction(node);
    key = Fnv1a64Value(key, junction != nullptr);
    if (!junction)
        return key;
    key = Fnv1a64Value(key, junction->State);
    key = Fnv1a64Value(key, junction->RoadAboveMeters);
    return Fnv1a64(NameView(world, junction->PavedBy), Fnv1a64Value(key, junction->PavedBy));
}

} // namespace

void SplineJunctionMarkers::Render(GizmoRenderContext& context, const ECS::World& world,
                                   SplineInteractionState& state)
{
    const auto& settings = SplineEditorSettings::Get();
    const bool constantScreen = settings.GetConstantScreenSize();
    const bool smartDistance = settings.GetSmartDistanceScaling();
    const bool lineSmart = constantScreen && smartDistance;
    const float32 thickness = settings.GetKnotThickness();
    const SplineSnapPreview& snap = state.SnapPreview();
    const float32 snapRadius =
        snap.Target.IsValid()
            ? MarkerRadius(ComputeSplineScreenScale(context, snap.Point, constantScreen, smartDistance))
            : 0.0f;

    if (const SplineECS::SplineNetwork* network = SplineECS::SplineNetwork::TryGet())
    {
        if (m_WorldId != world.GetWorldId() || m_WorldGeneration != world.GetLifecycleResetGeneration())
        {
            m_WorldId = world.GetWorldId();
            m_WorldGeneration = world.GetLifecycleResetGeneration();
            m_Seen.Clear();
            m_Primed = false;
        }
        RefreshHover(world);
        ++m_Frame;
        const auto now = std::chrono::steady_clock::now();
        GizmoDepthModeScope depthScope(context, GizmoDepthMode::AlwaysOnTop);
        for (const SplineNode& node : network->GetNodes(world))
        {
            Seen* seen = m_Seen.Find(node.Id);
            if (!seen)
                seen = &m_Seen.InsertOrAssign(node.Id,
                                              Seen{m_Primed ? now : std::chrono::steady_clock::time_point{}, 0});
            seen->Frame = m_Frame;
            if (snapRadius > 0.0f && (node.Position - snap.Point).Length() <= snapRadius * kSnapHidesMarkerRadii)
                continue;

            const bool settling = now - seen->First < kFirstSettleDuration;
            const bool heightsDiffer = SplineRecipes::JunctionHeightsDiffer(node, FindPavingJunction(node));
            const Color& color = heightsDiffer ? kHeightsDifferColor : settling ? kAttachColor : kJoinedColor;
            const float32 lineThickness =
                node.Id == m_HoveredNodeId ? thickness * kEmphasisThicknessScale : thickness;
            const Vector3 normal = SplineCirclePlaneNormal(context, node.Position);
            const float32 radius =
                MarkerRadius(ComputeSplineScreenScale(context, node.Position, constantScreen, smartDistance));
            GlyphFrame frame{node.Position, FirstMemberForward(node), {}, radius * kGlyphScale};
            frame.Across = Vector3::Cross(normal, frame.Along);
            if (frame.Across.LengthSquared() > 1e-8f)
                frame.Across = frame.Across * (1.0f / frame.Across.Length());
            else
                BuildPlaneBasis(normal, frame.Along, frame.Across);
            DrawMarker(context, node.Kind, frame, normal, radius,
                       LinePen{color, lineThickness, constantScreen, lineSmart});
        }
        m_Seen.EraseIf([frame = m_Frame](uint32, const Seen& entry) { return entry.Frame != frame; });
        m_Primed = true;
    }

    if (snapRadius <= 0.0f)
        return;
    // The snap ring draws with the handles, so the selected spline's band does not dim it.
    GizmoDepthModeScope depthScope(context, GizmoDepthMode::Handle);
    const Vector3 normal = SplineCirclePlaneNormal(context, snap.Point);
    const float32 ringThickness = thickness * kEmphasisThicknessScale;
    DrawThickWireCircle(context, snap.Point, normal, snapRadius, kBackingColor, ringThickness + kBackingExtraThickness,
                        constantScreen, lineSmart);
    DrawThickWireCircle(context, snap.Point, normal, snapRadius, kAttachColor, ringThickness, constantScreen,
                        lineSmart);
}

void SplineJunctionMarkers::UpdateHover(const ECS::World& world, const GizmoRay& ray)
{
    m_HoverRay = ray;
    m_HasHoverRay = true;
    RefreshHover(world);
}

void SplineJunctionMarkers::RefreshHover(const ECS::World& world)
{
    m_HoveredNodeId = m_HasHoverRay ? Pick(world, m_HoverRay) : 0u;
    const SplineECS::SplineNetwork* network = SplineECS::SplineNetwork::TryGet();
    const SplineNode* node = network ? network->FindNode(world, m_HoveredNodeId) : nullptr;
    if (!node)
    {
        m_HoverText.clear();
        m_HasHoverText = false;
        return;
    }
    const uint64 key = HoverTextKey(world, *node);
    if (m_HasHoverText && key == m_HoverTextKey)
        return;
    GE_CPU_PROFILE_SCOPE("SplineJunctionMarkers.HoverText");
    m_HoverText = Tooltip(world, *node);
    m_HoverTextKey = key;
    m_HasHoverText = true;
}

uint32 SplineJunctionMarkers::Pick(const ECS::World& world, const GizmoRay& ray)
{
    const SplineECS::SplineNetwork* network = SplineECS::SplineNetwork::TryGet();
    if (!network)
        return 0;
    const auto& settings = SplineEditorSettings::Get();
    const std::span<const SplineNode> nodes = network->GetNodes(world);
    m_PickSpheres.clear();
    for (const SplineNode& node : nodes)
    {
        const float32 scale = PickScreenScale(ray, node.Position, settings.GetConstantScreenSize(),
                                              settings.GetSmartDistanceScaling());
        m_PickSpheres.push_back({node.Position, MarkerRadius(scale)});
    }
    const std::optional<std::size_t> picked = PickNearestGizmoIcon(ray, m_PickSpheres);
    return picked ? nodes[*picked].Id : 0u;
}

std::string SplineJunctionMarkers::Tooltip(const ECS::World& world, const SplineNode& node)
{
    const std::string first = EntityDisplayName(world, node.Members[0]);
    const std::string second = EntityDisplayName(world, node.Members[1]);
    const std::string state = NodeState(world, node);
    switch (node.Kind)
    {
        case SplineNodeKind::Crossing:
            return std::format("Crossing · {} and {} · {}", first, second, state);
        case SplineNodeKind::Attachment:
            return std::format("T junction · {} on {} · {}", first, second, state);
        case SplineNodeKind::Entry:
            return std::format("Entry · {} into {} · {}", first, second, state);
        case SplineNodeKind::Merge:
            return std::format("Ends meet · {} and {} · {}", first, second, state);
    }
    return {};
}

} // namespace GameEngine::Editor::SceneTools
