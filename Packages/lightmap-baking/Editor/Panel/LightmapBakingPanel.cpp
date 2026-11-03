#include "Panel/LightmapBakingPanel.h"

#include "Service/LightmapBakeService.h"
#include "Service/LightmapBakeSelection.h"
#include "Assets/AssetManager.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/RenderServices.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Toggle.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "UI/InspectorSection.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"
#include "UI/StyleProperties.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <unordered_map>

namespace GameEngine
{
namespace
{
using json = nlohmann::json;
void EnableSubtree(UIElement& element,bool enabled)
{
    element.SetEnabled(enabled);
    for(const auto& child:element.GetChildren()) EnableSubtree(*child,enabled);
}

Label* AddLabel(UIElement& parent, const std::string& text)
{
    auto label=std::make_unique<Label>();
    label->SetText(text); label->AddClass("inspector-text");
    auto* result=label.get(); parent.AddChild(std::move(label)); return result;
}
Button* AddButton(UIElement& parent, const char* id, const char* text, std::function<void()> action)
{
    auto button=std::make_unique<Button>();
    button->SetId(id); button->SetText(text); button->AddClass("inspector-button");
    button->RegisterEventHandler(kEventButtonClick,[action=std::move(action)](UIEvent&) { action(); });
    auto* result=button.get(); parent.AddChild(std::move(button)); return result;
}
UIElement* AddSection(UIElement& parent, const char* title, bool collapsed=false)
{
    auto section=std::make_unique<InspectorSection>(title);
    section->SetShowEnabledToggle(false);
    section->SetCollapsed(collapsed);
    auto* body=section->GetContentRoot();
    parent.AddChild(std::move(section));
    return body;
}
IntField* AddInt(UIElement& parent, const char* id, const char* label, int value, int maximum)
{
    auto* field=InspectorDrag::AddIntRowWithDrag(&parent,label,value,[](int) {},[](int) {});
    field->SetId(id); field->SetRange(1,maximum); return field;
}
FloatField* AddFloat(UIElement& parent, const char* id, const char* label, float value, float maximum)
{
    auto* field=InspectorDrag::AddFloatRowWithDrag(&parent,label,value,[](float) {},[](float) {},value,nullptr,0,maximum);
    field->SetId(id); return field;
}
Toggle* AddToggle(UIElement& parent, const char* id, const char* label, bool value)
{
    auto* field=InspectorDrag::AddToggleRow(&parent,label,value,[](bool) {});
    field->SetId(id); return field;
}
bool IsStaticCaster(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* mesh=world.GetComponent<Components::MeshRenderer>(entity);
    return mesh && !world.HasComponent<ECS::ComponentDisabled<Components::MeshRenderer>>(entity) && mesh->castShadows && !world.GetComponent<ECS::Disabled>(entity) &&
        !world.GetComponent<Components::SkinnedMeshRenderer>(entity);
}
std::string EntityName(ECS::World& world, uint32_t id)
{
    const auto* name=world.GetComponent<Components::Name>(ECS::EntityHandle(id));
    return name && name->value[0] ? std::string(name->value) : "Unnamed mesh";
}
const Engine::Renderer::ExtractedLight* PrimaryLight(ECS::World* world)
{
    auto* services=EngineCore::GetInstance().GetRenderServices();
    return world && services ? Engine::Renderer::SelectPrimaryDirectional(services->GetWorldLights(world->GetWorldId())) : nullptr;
}
}

LightmapBakingPanel::LightmapBakingPanel() : DockPanel("Lighting Bake")
{
    AddClass("inspector-panel");
    auto scroll=std::make_unique<ScrollView>();
    scroll->SetId("LightmapSettingsScroll"); scroll->AddClass("inspector-scrollview");
    auto content=std::make_unique<UIElement>(); m_Settings=content.get();
    auto* meshes=AddSection(*content,"Meshes");
    auto* sceneActions=InspectorUI::AddRow(meshes);
    AddButton(*sceneActions,"LightmapAddSceneReceivers","Add scene receivers",[this] {
        std::vector<std::string> skipped;
        const auto receivers=Editor::CollectSceneLightmapReceivers(skipped);
        std::set<uint32_t> merged(m_Receivers.begin(),m_Receivers.end());
        merged.insert(receivers.begin(),receivers.end());
        if (merged.size()>4096) { ShowMessage("Bake at most 4096 receivers at a time.",true); return; }
        m_Receivers.assign(merged.begin(),merged.end());
        ShowMessage(receivers.empty()?"No eligible scene receivers. Meshes need lightmap UVs and an opaque Standard PBR material.":
            "Added "+std::to_string(receivers.size())+" scene receivers."+
            (skipped.empty()?"":" Skipped "+std::to_string(skipped.size())+" unsupported meshes; hover for details."));
        std::string reasons;
        for (const auto& reason:skipped) { if (!reasons.empty()) reasons+="\n"; reasons+=reason; }
        m_Status->SetTooltip(reasons);
        RefreshMeshLabels(); m_NextRefresh={}; Update();
    })->SetTooltip("Add enabled static scene meshes with lightmap UVs and supported opaque materials. Existing bakes can be refreshed.");
    auto* receiverActions=InspectorUI::AddRow(meshes);
    AddButton(*receiverActions,"LightmapAddReceivers","Add selected receivers",[this] { AddSelection(true); });
    AddButton(*receiverActions,"LightmapClearReceivers","Clear",[this] { m_Receivers.clear(); RefreshMeshLabels(); });
    m_ReceiverLabel=AddLabel(*meshes,"No receivers. Select meshes or a model in the hierarchy.");
    m_ReceiverLabel->SetId("LightmapReceivers");
    m_AllOccluders=AddToggle(*meshes,"LightmapAllOccluders","Scene occluders",true);
    m_AllOccluders->SetTooltip("Include enabled static meshes that cast shadows, for occlusion and bounced lighting. Disable to choose them explicitly.");
    auto custom=std::make_unique<UIElement>(); m_CustomOccluders=custom.get();
    auto* casterActions=InspectorUI::AddRow(custom.get());
    AddButton(*casterActions,"LightmapAddOccluders","Add selected occluders",[this] { AddSelection(false); });
    AddButton(*casterActions,"LightmapClearOccluders","Clear",[this] { m_Occluders.clear(); RefreshMeshLabels(); });
    meshes->AddChild(std::move(custom));
    m_OccluderLabel=AddLabel(*meshes,"Receivers also cast shadows and bounce light.");
    m_OccluderLabel->SetId("LightmapOccluders");
    m_LightLabel=AddLabel(*meshes,"Directional light: waiting for the scene.");
    m_AllOccluders->SetOnValueChanged([this](bool) { RefreshMeshLabels(); });

    auto* quality=AddSection(*content,"Quality");
    m_Resolution=InspectorUI::AddDropdownRow(quality,"Resolution",
        {{"64","64 × 64 · preview"},{"256","256 × 256"},{"512","512 × 512"},{"1024","1024 × 1024"},{"2048","2048 × 2048"},{"4096","4096 × 4096"},{"8192","8192 × 8192"}},3);
    m_Resolution->SetId("LightmapResolution");
    m_GI=AddToggle(*quality,"LightmapGI","Diffuse GI",true);
    m_Samples=AddInt(*quality,"LightmapSamples","GI samples",256,4096);
    m_Bounces=AddInt(*quality,"LightmapBounces","Light bounces",2,16);
    m_Denoise=AddToggle(*quality,"LightmapDenoise","Denoise",true);
    m_GI->SetOnValueChanged([this](bool enabled) {
        EnableSubtree(*m_Samples,enabled); EnableSubtree(*m_Bounces,enabled); EnableSubtree(*m_Denoise,enabled);
    });
    m_ShadowMode=InspectorUI::AddDropdownRow(quality,"Shadows",
        {{"Overlay","Baked + live"},{"Replace","Fade to baked"},{"Only","Baked only"}},0,
        "Baked + live keeps static shadows at all distances. Fade to baked uses live shadows nearby. Baked only cannot show moving shadows.");
    m_ShadowMode->SetId("LightmapShadowMode");

    auto* advanced=AddSection(*content,"Advanced",true);
    m_ShadowSamples=AddInt(*advanced,"LightmapShadowSamples","Shadow samples",32,1024);
    m_FadeStart=AddFloat(*advanced,"LightmapFadeStart","Fade start",40,100000);
    m_FadeEnd=AddFloat(*advanced,"LightmapFadeEnd","Fade end",50,100000);
    m_FadeEnd->SetTooltip("Keep the fade range inside the scene's live shadow distance.");
    m_Ambient=AddFloat(*advanced,"LightmapAmbient","Ambient light",0,10000);
    auto* environmentRow=InspectorUI::AddRow(advanced);
    InspectorUI::AddLabel(environmentRow,"Environment image");
    auto environment=std::make_unique<TextField>(); m_Environment=environment.get();
    m_Environment->SetId("LightmapEnvironment");
    m_Environment->SetTooltip("Optional panorama path, relative to project assets or absolute. Overrides the uniform ambient light.");
    InspectorUI::AddFieldContainer(environmentRow)->AddChild(std::move(environment));
    m_EnvironmentIntensity=AddFloat(*advanced,"LightmapEnvironmentIntensity","Image intensity",1,10000);
    AddLabel(*advanced,"The scene sky is not captured automatically. Add ambient light or a panorama to include it in the bake.");
    scroll->AddContent(std::move(content)); AddChild(std::move(scroll));

    auto footer=std::make_unique<UIElement>(); footer->AddClass("inspector-bottom");
    auto footerContent=std::make_unique<UIElement>(); footerContent->AddClass("inspector-section-body");
    m_Status=AddLabel(*footerContent,"Add receiver meshes with non-overlapping lightmap UVs."); m_Status->SetId("LightmapStatus");
    m_ProgressLabel=AddLabel(*footerContent,"Ready"); m_ProgressLabel->SetId("LightmapProgressText");
    auto* actions=InspectorUI::AddRow(footerContent.get());
    m_Bake=AddButton(*actions,"LightmapBake","Bake lighting",[this] { StartBake(); });
    m_Cancel=AddButton(*actions,"LightmapCancel","Cancel",[this] { if(m_Service) m_Service->Cancel(); });
    m_Cancel->SetEnabled(false);
    footer->AddChild(std::move(footerContent));
    AddChild(std::move(footer));
    m_CustomOccluders->Overrides().Set(Style::Display,DisplayMode::None);
    m_Bake->SetEnabled(false);
}

LightmapBakingPanel::~LightmapBakingPanel()
{
    if (OnDestroyed)
        OnDestroyed();
}

void LightmapBakingPanel::SetServices(std::shared_ptr<Editor::LightmapBakeService> service,
    std::function<std::vector<uint32_t>()> selection, std::function<bool()> canBake)
{
    if (m_Service != service)
    {
        m_Receivers.clear(); m_Occluders.clear();
        m_LastResult.clear(); m_ActiveJob.clear(); m_NextRefresh={};
    }
    m_Service=std::move(service); m_GetSelection=std::move(selection);
    m_CanBake=std::move(canBake);
    if (!m_Service)
    {
        m_Bake->SetEnabled(false);
        m_Cancel->SetEnabled(false);
        return;
    }
    m_SceneVersion=m_Service->SceneVersion();
    m_ProjectRoot=EngineCore::GetInstance().GetWorkspaceRoot().string();
    Update();
}

std::vector<uint32_t> LightmapBakingPanel::SceneOccluders() const
{
    std::vector<uint32_t> result;
    if(auto* world=EngineCore::GetInstance().GetPrimaryWorld())
        world->Query<ECS::Read<Components::MeshRenderer>>().Each([&](ECS::EntityHandle entity,const Components::MeshRenderer&) {
            if(IsStaticCaster(*world,entity)) result.push_back(entity.id);
        });
    std::sort(result.begin(),result.end()); return result;
}

std::string LightmapBakingPanel::MeshNames(const std::vector<uint32_t>& ids) const
{
    auto* world=EngineCore::GetInstance().GetPrimaryWorld();
    if(ids.empty() || !world) return "None selected";
    std::string text=std::to_string(ids.size())+(ids.size()==1?" mesh: ":" meshes: ");
    for(size_t i=0;i<std::min(ids.size(),size_t(4));++i)
    {
        if(i) text+=", ";
        text+=world->IsValid(ECS::EntityHandle(ids[i])) ? EntityName(*world,ids[i]) : "Removed mesh";
    }
    if(ids.size()>4) text+=" …";
    return text;
}

void LightmapBakingPanel::RefreshMeshLabels()
{
    m_ReceiverLabel->SetText(m_Receivers.empty()?"No receivers. Select meshes or a model in the hierarchy.":MeshNames(m_Receivers));
    m_CustomOccluders->Overrides().Set(Style::Display,m_AllOccluders->GetValue()?DisplayMode::None:DisplayMode::Flex);
    m_OccluderLabel->SetText(m_AllOccluders->GetValue()?MeshNames(SceneOccluders()):
        (m_Occluders.empty()?"Only receivers cast shadows and bounce light.":MeshNames(m_Occluders)));
}

void LightmapBakingPanel::AddSelection(bool receivers)
{
    auto* world=EngineCore::GetInstance().GetPrimaryWorld();
    if(!world || !m_GetSelection) return;
    const auto selected=m_GetSelection();
    std::vector<uint32_t> pending=selected;
    std::set<uint32_t> visited;
    std::unordered_map<uint32_t,std::vector<uint32_t>> children;
    world->Query<ECS::Read<Components::Parent>>().Each([&](ECS::EntityHandle entity,const Components::Parent& p) {
        children[p.parent.id].push_back(entity.id);
    });
    auto& destination=receivers?m_Receivers:m_Occluders;
    std::set<uint32_t> result(destination.begin(),destination.end());
    size_t accepted=0;
    while(!pending.empty())
    {
        const uint32_t id=pending.back(); pending.pop_back();
        if(!visited.insert(id).second || !world->IsValid(ECS::EntityHandle(id))) continue;
        const auto& descendants=children[id]; pending.insert(pending.end(),descendants.begin(),descendants.end());
        if(IsStaticCaster(*world,ECS::EntityHandle(id))) { result.insert(id); ++accepted; }
    }
    if(!accepted) { ShowMessage("Select enabled static meshes, or a model containing them, in the hierarchy. Meshes must cast shadows.",true); return; }
    if(result.size()>4096u) { ShowMessage(receivers?"Bake at most 4096 receivers at a time.":"Choose at most 4096 occluders.",true); return; }
    destination.assign(result.begin(),result.end());
    ShowMessage("Selection added. Baking will check the meshes, materials and lightmap UVs.");
    RefreshMeshLabels(); m_NextRefresh={}; Update();
}

void LightmapBakingPanel::ShowMessage(const std::string& text,bool error)
{
    m_Status->SetText(text);
    m_Status->SetTooltip("");
    if(error) m_Status->AddClass("inspector-warning"); else m_Status->RemoveClass("inspector-warning");
}

void LightmapBakingPanel::StartBake()
{
    if(!m_Service || (m_CanBake && !m_CanBake())) { ShowMessage("Stop playback before baking lighting.",true); return; }
    auto* world=EngineCore::GetInstance().GetPrimaryWorld();
    const auto* primary=PrimaryLight(world);
    if(!primary) { ShowMessage("Add an enabled directional light to the scene before baking.",true); return; }
    const float ambient=m_Ambient->GetValue();
    json request{{"receivers",m_Receivers},{"occluders",m_AllOccluders->GetValue()?SceneOccluders():m_Occluders},
        {"lightEntity",primary->SortId},{"resolution",std::stoi(m_Resolution->GetSelectedValue())},
        {"giSamples",m_GI->GetValue()?m_Samples->GetValue():0},{"bounces",m_Bounces->GetValue()},
        {"denoise",m_Denoise->GetValue()},{"shadowSamples",m_ShadowSamples->GetValue()},
        {"shadowMode",m_ShadowMode->GetSelectedValue()},{"fadeStart",m_FadeStart->GetValue()},{"fadeEnd",m_FadeEnd->GetValue()},
        {"environmentRadiance",{ambient,ambient,ambient}},{"apply",true}};
    if(!m_Environment->GetValue().empty())
    {
        request["environmentTexture"]=m_Environment->GetValue();
        request["environmentIntensity"]=m_EnvironmentIntensity->GetValue();
    }
    const auto result=m_Service->Start(request);
    if(result.contains("error")) ShowMessage(result["error"].get<std::string>(),true);
    else { m_LastResult.clear(); ShowMessage("Baking lighting. You can keep working; changing bake inputs prevents automatic assignment."); }
    m_NextRefresh={}; Update();
}

void LightmapBakingPanel::Update()
{
    if(!m_Service || std::chrono::steady_clock::now()<m_NextRefresh) return;
    m_NextRefresh=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);
    const auto project=EngineCore::GetInstance().GetWorkspaceRoot().string();
    if(m_SceneVersion!=m_Service->SceneVersion() || project!=m_ProjectRoot)
    {
        m_SceneVersion=m_Service->SceneVersion(); m_ProjectRoot=project;
        m_Receivers.clear(); m_Occluders.clear(); m_LastResult.clear();
        ShowMessage("Scene changed. Select the meshes to bake.");
    }
    const auto result=m_Service->Poll();
    const std::string status=result.value("status",std::string("idle"));
    const bool busy=status=="baking", cancelling=result.value("cancelling",false);
    const bool editing=!m_CanBake || m_CanBake();
    auto* world=EngineCore::GetInstance().GetPrimaryWorld();
    const auto* light=PrimaryLight(world);
    if(m_ControlsEnabled!=(!busy && editing))
    {
        m_ControlsEnabled=!busy && editing;
        EnableSubtree(*m_Settings,m_ControlsEnabled);
        const bool gi=m_ControlsEnabled && m_GI->GetValue();
        EnableSubtree(*m_Samples,gi); EnableSubtree(*m_Bounces,gi); EnableSubtree(*m_Denoise,gi);
    }
    m_Bake->SetEnabled(!busy && editing && !m_Receivers.empty() && light);
    m_Cancel->SetEnabled(busy && !cancelling);
    m_LightLabel->SetText(light && world?"Directional light: "+EntityName(*world,light->SortId):"No active directional light. Add one to the scene.");
    const float progress=std::clamp(result.value("progress",status=="complete"?1.0f:0.0f),0.0f,1.0f);
    if(busy)
    {
        const auto jobId=result.value("jobId",std::string{});
        if(jobId!=m_ActiveJob)
        {
            m_ActiveJob=jobId;
            ShowMessage("Baking lighting. You can keep working; changing bake inputs prevents automatic assignment.");
        }
        m_ProgressLabel->SetText((cancelling?"Cancelling · ":"Baking · ")+std::to_string(int(progress*100))+"% · "+
            (result.value("acceleration",std::string("cpu"))=="gpu_ray_query"?"GPU · ":"CPU · ")+
            std::to_string(result.value("completedReceivers",0u))+" / "+std::to_string(result.value("totalReceivers",0u))+" meshes complete");
    }
    else
    {
        m_ProgressLabel->SetText(!editing?"Stop playback to bake lighting":status=="complete"?"Bake complete":status=="cancelled"?"Cancelled":"Ready");
        const auto key=result.dump();
        if(key!=m_LastResult)
        {
            m_LastResult=key;
            const bool applied=status=="complete" && result.value("applied",false);
            if(applied) ShowMessage("Baked materials applied.");
            else if(status=="complete") ShowMessage(result.value("reason",std::string("Baked files saved without assigning materials.")),true);
            else if(status=="failed") ShowMessage(result.value("error",std::string("The bake failed.")),true);
            else if(status=="cancelled") ShowMessage("Bake cancelled. Existing materials were kept.");
        }
    }
    RefreshMeshLabels();
}
}
