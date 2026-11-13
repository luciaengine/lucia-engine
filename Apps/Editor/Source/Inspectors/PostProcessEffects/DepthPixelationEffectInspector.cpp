#include "Inspectors/PostProcessEffects/DepthPixelationEffectInspector.h"
#include "InspectorRegistry.h"
#include "Components/Rendering/PostProcessEffects/DepthPixelationEffect.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include <algorithm>
#include <charconv>
#include <vector>

namespace GameEngine {
void RegisterDepthPixelationEffectInspector()
{
    using namespace InspectorDrag;
    using Effect = Components::DepthPixelationEffect;
    InspectorFn fn = [](const InspectorContext& ctx) {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid()) return;
        auto* c = ctx.World->GetComponent<Effect>(ctx.Entity);
        if (!c) return;
        auto* w = ctx.World; auto e = ctx.Entity; auto* n = ctx.ChangeNotifications; auto* undo = ctx.Undo;
        auto rebuild = [w, e, n] {
            if (!n) return;
            Editor::EditorChangeNotifications::ComponentChangedEvent ev{};
            ev.world = w; ev.entity = e; ev.componentType = ECS::GetComponentTypeId<Effect>();
            ev.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
            n->NotifyComponentChanged(ev);
        };
        auto number = [&](const std::string& label, float Effect::*member, float defaultValue,
                          const char* tooltip, float minimum, float maximum) {
            AddComponentFloatRowWithDrag<Effect>(ctx.Parent, label, c->*member, w, e, n, undo,
                "Change Pixelation " + label, [member, minimum, maximum](Effect& x, float v) {
                    x.*member = std::clamp(v, minimum, maximum);
                }, defaultValue, tooltip, {}, minimum, maximum);
        };
        static const std::vector<Dropdown::Option> counts = {
            {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"},
            {"6", "6"}, {"7", "7"}, {"8", "8"}, {"9", "9"}, {"10", "10"}
        };
        auto* count = InspectorUI::AddDropdownRow(ctx.Parent, "Band Count", counts, std::clamp(c->BandCount, 1, 10) - 1,
            "Number of distance bands, from 1 to 10. Sky has a separate setting.");
        count->SetOnValueChanged([w, e, n, undo, rebuild](const std::string& value) {
            int selected = 3;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), selected);
            if (parsed.ec != std::errc{}) return;
            CommitComponentWithUndo<Effect>(w, e, n, undo, "Change Pixelation Band Count",
                [selected](Effect& x) { x.BandCount = std::clamp(selected, 1, 10); });
            rebuild();
        });
        static const std::vector<Dropdown::Option> shapes = {{"0", "Straight (Depth)"}, {"1", "Curved (Radial)"}};
        auto* shape = InspectorUI::AddDropdownRow(ctx.Parent, "Band Shape", shapes, std::clamp(c->DistanceMode, 0, 1),
            "Straight bands use forward camera depth; curved bands use radial distance from the camera");
        shape->SetOnValueChanged([w, e, n, undo, rebuild](const std::string& value) {
            CommitComponentWithUndo<Effect>(w, e, n, undo, "Change Pixelation Band Shape",
                [value](Effect& x) { x.DistanceMode = value == "1" ? 1 : 0; });
            rebuild();
        });
        AddToggleRow(ctx.Parent, "Simple Method", c->SimpleScreenSpace != 0,
            [w, e, n, undo, rebuild](bool enabled) {
                CommitComponentWithUndo<Effect>(w, e, n, undo, "Change Pixelation Method",
                    [enabled](Effect& x) { x.SimpleScreenSpace = enabled ? 1 : 0; });
                rebuild();
            }, "Compare depth-masked pixelation of the rendered image with separately rendered bands. Keeps the same sizing and stabilization settings.");
        static const std::vector<Dropdown::Option> stabilizationModes = {{"0", "Off"}, {"1", "Auto"}};
        auto* stabilization = InspectorUI::AddDropdownRow(ctx.Parent, "Pixel-grid Stabilization", stabilizationModes,
            std::clamp(c->StabilizationMode, 0, 1),
            "Auto snaps the render projection to its pixel grid and compensates sampling while the display grid remains screen-aligned.");
        stabilization->SetOnValueChanged([w, e, n, undo](const std::string& value) {
            CommitComponentWithUndo<Effect>(w, e, n, undo, "Change Pixel-grid Stabilization",
                [value](Effect& x) { x.StabilizationMode = value == "1" ? 1 : 0; });
        });
        if (std::clamp(c->BandCount, 1, 10) == 1)
        {
            // Existing automatic one-band scenes use 1x/2x pixels. Display
            // that effective value until the user chooses a manual size.
            const bool automatic = c->AutoBandResolution != 0;
            const bool scaled = automatic || c->ResolutionMode == 1;
            static const std::vector<Dropdown::Option> sizeModes = {
                {"0", "Screen Size"}, {"1", "Pixel Scale"}
            };
            auto* sizeMode = InspectorUI::AddDropdownRow(ctx.Parent, "Pixelation Size", sizeModes, scaled ? 1 : 0,
                "Choose a fixed screen resolution or scale each pixel relative to the viewport. Applies to the scene and sky.");
            sizeMode->SetOnValueChanged([w, e, n, undo, rebuild](const std::string& value) {
                CommitComponentWithUndo<Effect>(w, e, n, undo, "Change Pixelation Size Mode",
                    [value](Effect& x) {
                        if (x.AutoBandResolution != 0)
                            x.NearDivisor = static_cast<float>(std::clamp(x.AutoFarPixelSize, 1, 2));
                        x.AutoBandResolution = 0;
                        x.ResolutionMode = value == "1" ? 1 : 0;
                        x.SkyDivisor = x.NearDivisor;
                        x.SkyResolution = x.NearResolution;
                    });
                rebuild();
            });
            const float currentSize = scaled
                ? (automatic ? static_cast<float>(std::clamp(c->AutoFarPixelSize, 1, 2)) : c->NearDivisor)
                : c->NearResolution;
            if (scaled)
            {
                AddComponentIntRowWithDrag<Effect>(ctx.Parent, "Pixel Scale",
                    static_cast<int>(std::clamp(currentSize, 1.0f, 16384.0f)), w, e, n, undo,
                    "Change Pixelation Size",
                    [](Effect& x, int value) {
                        x.AutoBandResolution = 0;
                        x.ResolutionMode = 1;
                        x.NearDivisor = x.SkyDivisor = static_cast<float>(std::clamp(value, 1, 16384));
                    }, 4, "Whole screen pixels per rendered pixel. Applies to scene and sky.");
            }
            else
            {
                AddComponentFloatRowWithDrag<Effect>(ctx.Parent, "Screen Height",
                    currentSize, w, e, n, undo, "Change Pixelation Size",
                    [](Effect& x, float value) {
                        x.AutoBandResolution = 0;
                        x.ResolutionMode = 0;
                        x.NearResolution = x.SkyResolution = std::clamp(value, 1.0f, 16384.0f);
                    }, 360.0f,
                    "Vertical rendering resolution in pixels. Width follows the viewport aspect ratio. Applies to scene and sky.",
                    {}, 1.0f, 16384.0f);
            }
            AddToggleRow(ctx.Parent, "Debug Depth Ranges", c->DebugRanges != 0,
                [w, e, n, undo](bool enabled) {
                    CommitComponentWithUndo<Effect>(w, e, n, undo, "Toggle Pixelation Depth Ranges",
                        [enabled](Effect& x) { x.DebugRanges = enabled ? 1 : 0; });
                }, "Show scene coverage in green and sky in purple");
            return;
        }
        AddToggleRow(ctx.Parent, "Automatic Band Pixels", c->AutoBandResolution != 0,
            [w, e, n, undo, rebuild](bool enabled) {
                CommitComponentWithUndo<Effect>(w, e, n, undo, "Toggle Automatic Band Pixels",
                    [enabled](Effect& x) { x.AutoBandResolution = enabled ? 1 : 0; });
                rebuild();
            }, "Use the current screen resolution. Each band closer to the camera doubles the pixel size. Sky matches the farthest band.");
        if (c->AutoBandResolution != 0)
        {
            static const std::vector<Dropdown::Option> farPixels = {{"1", "1× pixel"}, {"2", "2× pixels"}};
            auto* farSize = InspectorUI::AddDropdownRow(ctx.Parent, "Farthest Band Pixels", farPixels,
                std::clamp(c->AutoFarPixelSize, 1, 2) - 1, "Size of the farthest band's pixels in screen pixels");
            farSize->SetOnValueChanged([w, e, n, undo, rebuild](const std::string& value) {
                CommitComponentWithUndo<Effect>(w, e, n, undo, "Change Farthest Band Pixels",
                    [value](Effect& x) { x.AutoFarPixelSize = value == "2" ? 2 : 1; });
                rebuild();
            });
        }
        else
        {
            static const std::vector<Dropdown::Option> modes = {{"0", "Fixed Pixel Height"}, {"1", "Screen Divisor"}};
            auto* mode = InspectorUI::AddDropdownRow(ctx.Parent, "Resolution Mode", modes, std::clamp(c->ResolutionMode, 0, 1),
                "Use a fixed pixel height or divide the current viewport dimensions");
            mode->SetOnValueChanged([w, e, n, undo, rebuild](const std::string& value) {
                CommitComponentWithUndo<Effect>(w, e, n, undo, "Change Pixelation Resolution Mode",
                    [value](Effect& x) { x.ResolutionMode = value == "1" ? 1 : 0; });
                rebuild();
            });
        }
        static constexpr float Effect::* ends[] = {
            &Effect::NearEnd, &Effect::MiddleEnd, &Effect::Band3End, &Effect::Band4End,
            &Effect::Band5End, &Effect::Band6End, &Effect::Band7End, &Effect::Band8End, &Effect::Band9End
        };
        static constexpr float Effect::* resolutions[] = {
            &Effect::NearResolution, &Effect::MiddleResolution, &Effect::FarResolution,
            &Effect::Band4Resolution, &Effect::Band5Resolution, &Effect::Band6Resolution,
            &Effect::Band7Resolution, &Effect::Band8Resolution, &Effect::Band9Resolution, &Effect::Band10Resolution
        };
        static constexpr float Effect::* divisors[] = {
            &Effect::NearDivisor, &Effect::MiddleDivisor, &Effect::FarDivisor,
            &Effect::Band4Divisor, &Effect::Band5Divisor, &Effect::Band6Divisor,
            &Effect::Band7Divisor, &Effect::Band8Divisor, &Effect::Band9Divisor, &Effect::Band10Divisor
        };
        const Effect defaults{};
        const int bandCount = std::clamp(c->BandCount, 1, 10);
        const bool divided = c->ResolutionMode == 1;
        for (int i = 0; i < bandCount; ++i)
        {
            const std::string band = "Band " + std::to_string(i + 1);
            if (i + 1 < bandCount)
                number(band + " End", ends[i], defaults.*ends[i],
                    c->DistanceMode == 1 ? "Radial camera distance where the next band begins, in world units"
                                         : "Forward camera depth where the next band begins, in world units", 0, 1000000);
            if (c->AutoBandResolution != 0)
            {
                const int pixels = std::clamp(c->AutoFarPixelSize, 1, 2) * (1 << (bandCount - 1 - i));
                InspectorUI::AddLabel(InspectorUI::AddRow(ctx.Parent), band + " Pixels: " + std::to_string(pixels) + "×");
            }
            else
            {
                const auto member = divided ? divisors[i] : resolutions[i];
                number(band + (divided ? " Divisor" : " Resolution"), member, defaults.*member,
                    divided ? "Screen width and height divided by this value; larger values make larger pixels"
                            : "Vertical pixel count; lower values make larger pixels", 1, 16384);
            }
        }
        if (c->AutoBandResolution == 0)
        {
            const auto skyMember = divided ? &Effect::SkyDivisor : &Effect::SkyResolution;
            number(divided ? "Sky Divisor" : "Sky Resolution", skyMember, defaults.*skyMember,
                divided ? "Divide screen dimensions for the sky" : "Vertical pixel count for the sky", 1, 16384);
        }
        AddToggleRow(ctx.Parent, "Debug Depth Ranges", c->DebugRanges != 0,
            [w, e, n, undo](bool enabled) {
                CommitComponentWithUndo<Effect>(w, e, n, undo, "Toggle Pixelation Depth Ranges",
                    [enabled](Effect& effect) { effect.DebugRanges = enabled ? 1 : 0; });
            }, "Show each distance band in a distinct color; sky is purple");
    };
    InspectorRegistry::Get().RegisterComponentInspector<Effect>(std::move(fn));
}
}
