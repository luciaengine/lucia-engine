#include "Editor/Settings/ScriptEditorSettings.h"

#include "Logger/Logger.h"
#include "Rendering/Text/FontAtlas.h"
#include "Types/StringId.h"
#include "UI/EditorUIFontSettings.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

// Size the script face's line metrics are probed at. A `normal` line box is
// rounded to whole pixels per size, so the ratio derived below is an
// approximation of the box at the stylesheet's size rather than an identity;
// probing large keeps the rounding a small fraction of the result.
constexpr unsigned kScriptFontMetricProbePx = 64u;

// The line-height slider steps by 0.05, so no legitimate setting lands this
// close to 1.0 without meaning it.
constexpr float kScriptLineHeightNormalEpsilon = 0.001f;

// Font families resolve on the job system, so the startup apply can run before
// the selected script face exists. Re-resolve for a bounded number of drains
// rather than baking the fallback atlas's metrics for the rest of the session.
constexpr int kScriptFontResolveDrains = 120;

// What an apply needs, resolved once from disk by the public entry point so the
// re-resolve chain below never touches the preferences file again. Seq retires
// superseded chains: a later apply bumps it, and the older chain's next drain
// sees the mismatch and stops.
struct ScriptLineHeightApply
{
    float LineHeight = 1.0f;
    std::shared_ptr<const std::vector<std::string>> Families;
    int DrainsLeft = 0;
    uint32_t Seq = 0;
};
// Entries live only for the duration of a retry chain — a finished or
// exhausted chain erases its own, so a closed window's UIManager* cannot
// accumulate here.
std::unordered_map<UIManager*, ScriptLineHeightApply> s_ScriptLineHeightApplyByUi;
// Monotonic across entries rather than per-entry: entries are erased, so a
// per-entry counter would restart at 1 and a retired chain's queued callback
// could match a fresh chain's Seq.
uint32_t s_ScriptLineHeightApplySeq = 0;

struct ScriptFontNormalLine
{
    // Normal line height divided by font size for the resolved script face.
    float Ratio = 0.0f;
    // False while the selected family is still streaming in and
    // ResolveFontForStyle is answering with the default UI atlas.
    bool FromScriptFace = false;
};

ScriptFontNormalLine ResolveScriptFontNormalLine(
    UIManager* ui, std::shared_ptr<const std::vector<std::string>> families)
{
    ScriptFontNormalLine out;

    // ResolveFontForStyle keys on the family stack and the weight/style/variant
    // defaults the script rule inherits; the size only matters to the metric
    // query below.
    ResolvedStyle style;
    style.Visual.FontFamily = std::move(families);

    Rendering::Text::FontAtlas* atlas = ui->ResolveFontForStyle(style);
    if (!atlas)
        return out;

    const auto metrics = atlas->GetFontLineMetrics(kScriptFontMetricProbePx);
    out.Ratio = metrics.height / static_cast<float>(kScriptFontMetricProbePx);
    // A script face that genuinely shares the UI face's bytes aliases to the
    // same atlas and simply re-derives the same ratio, so treating it as
    // unresolved only costs a few idempotent re-applies.
    out.FromScriptFace = (atlas != ui->GetDefaultFontAtlas());
    return out;
}

// Returns true when the emitted value is provisional because the script face
// has not resolved yet.
bool ApplyScriptLineHeightStyle(UIManager* ui, const ScriptLineHeightApply& apply)
{
    UIElement* root = ui->GetRootElement();
    if (!root)
        return false;

    const float lineHeight = apply.LineHeight;

    // The saved value means "multiples of a normal line". CSS resolves a
    // unitless line-height against the font SIZE, so a fixed multiplier only
    // means one line for a face whose metric ratio happens to match it;
    // `normal` is the resolved face's own line height, which is exactly what
    // 1.0 has to reproduce for every selectable script font.
    std::string lineHeightCss = "normal";
    bool provisional = false;
    if (std::abs(lineHeight - 1.0f) > kScriptLineHeightNormalEpsilon)
    {
        const ScriptFontNormalLine normalLine = ResolveScriptFontNormalLine(ui, apply.Families);
        if (normalLine.Ratio > 0.0f)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", lineHeight * normalLine.Ratio);
            lineHeightCss = buf;
        }
        provisional = !normalLine.FromScriptFace;
    }

    // Keep script-variable input rows in Inspector visually in sync with the
    // Script Editor line-height slider. These track the multiplier itself, not
    // the script face — they are UI chrome, not script text.
    constexpr float kScriptInspectorBaseRowHeightPx = 27.0f;
    const float scriptInspectorRowHeightPx =
        std::clamp(kScriptInspectorBaseRowHeightPx * lineHeight, 16.0f, 128.0f);
    char rowBuf[32];
    std::snprintf(rowBuf, sizeof(rowBuf), "%.1fpx", scriptInspectorRowHeightPx);
    const std::string rowHeightCss = rowBuf;

    auto setCustom = [root](StringId name, const std::string& value)
    {
        const std::string* existing = root->Overrides().FindCustom(name);
        if (existing && *existing == value)
            return false;
        root->Overrides().SetCustom(name, value);
        return true;
    };

    bool changed = setCustom(HashStringId("--script_line_height"), lineHeightCss);
    changed |= setCustom(HashStringId("--script_inspector_row_height"), rowHeightCss);
    changed |= setCustom(HashStringId("--inspector-row-height"), rowHeightCss);
    if (changed)
        ui->MarkStyleDirtySubtree(root);

    return provisional;
}

// Both terminal paths below erase the map entry. The one case that does not is
// a window closing mid-chain: the UIManager's dispatcher dies with it, so the
// queued callback never runs and its entry outlives it. That leaks one struct
// per window closed inside the retry window (at most kScriptFontResolveDrains
// drains, ~2s), and cannot be fixed here — it needs a UIManager teardown
// notification, which the editor does not have. s_FontPrefsSheetByUi has the
// same residual for the same reason. Address reuse is safe: every field is
// overwritten on the next apply, and Seq is module-monotonic so a retired
// callback can never match a fresh chain.
void PumpScriptLineHeightRetry(UIManager* ui, uint32_t seq)
{
    ui->PostToUI(
        [ui, seq]()
        {
            auto it = s_ScriptLineHeightApplyByUi.find(ui);
            if (it == s_ScriptLineHeightApplyByUi.end() || it->second.Seq != seq ||
                it->second.DrainsLeft <= 0)
                return;

            --it->second.DrainsLeft;
            if (!ApplyScriptLineHeightStyle(ui, it->second))
            {
                s_ScriptLineHeightApplyByUi.erase(it);
                return;
            }
            if (it->second.DrainsLeft <= 0)
            {
                // Out of drains with the script face still unresolved: the
                // default UI atlas's ratio is what stays applied for the
                // session, so say so rather than leaving a silently wrong
                // line height.
                Logger::Log::Warning(
                    "ScriptEditorSettings: script font did not resolve within {} drains; script line "
                    "height stays on the default UI face's metrics",
                    kScriptFontResolveDrains);
                s_ScriptLineHeightApplyByUi.erase(it);
                return;
            }
            PumpScriptLineHeightRetry(ui, seq);
        });
}

} // namespace

void ScriptEditorSettings::SetLineHeight(float value)
{
    m_LineHeight = std::clamp(value, 0.5f, 4.0f);
    Save();
    ReapplyLineHeight();
}

void ScriptEditorSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    ApplyLineHeightToWindow(*ui);
}

void ScriptEditorSettings::ReapplyLineHeight()
{
    m_Windows.ForEachLive([this](UIManager& ui) { ApplyLineHeightToWindow(ui); });
}

void ScriptEditorSettings::ApplyLineHeightToWindow(UIManager& ui) const
{
    std::string scriptFontPreset;
    {
        SettingsStore prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        (void)prefs.TryGetString(kPrefScriptFontPreset, scriptFontPreset);
    }

    auto& apply = s_ScriptLineHeightApplyByUi[&ui];
    apply.LineHeight = m_LineHeight;
    apply.Families = std::make_shared<const std::vector<std::string>>(
        GetScriptFontFamilyStack(scriptFontPreset));
    apply.Seq = ++s_ScriptLineHeightApplySeq;
    if (!ApplyScriptLineHeightStyle(&ui, apply))
    {
        s_ScriptLineHeightApplyByUi.erase(&ui);
        return;
    }
    apply.DrainsLeft = kScriptFontResolveDrains;
    PumpScriptLineHeightRetry(&ui, apply.Seq);
}

} // namespace GameEngine::Editor
