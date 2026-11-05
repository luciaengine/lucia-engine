#include "UI/UIManager.h"

#include "Logger/Logger.h"
#include "Platform/SystemMetrics.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/CaretBlink.h"
#include "UI/UIFrameBufferRing.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

// RenderGraph pass of the SDF UI renderer — the only render path since the
// retained twin was deleted. Declared fresh every frame: no retained pass
// state, no bindingsHash/targetHash recreate machinery. The viewport is the
// engine's one orientation — a positive-height rect, clip +Y on its top row —
// and ui_sdf.vert maps its top-left-origin pixels into that. SdfPushConstants
// block: the UI shader outputs LINEAR, the terminal encode owns the only OETF.
int UIManager::ResolveOutputEncoding(UI::UITargetSpace targetSpace)
{
    static_assert(UI::UITargetSpace::kKindCount == 5,
                  "A new UITargetSpace needs an outputEncoding arm here.");
    switch (targetSpace.GetKind())
    {
    case UI::UITargetSpace::Kind::EncodedSrgb:
        return 1;
    case UI::UITargetSpace::Kind::HdrPq:
        return 2;
    case UI::UITargetSpace::Kind::Hlg:
        return 3;
    case UI::UITargetSpace::Kind::ScRgb:
        return 4;
    case UI::UITargetSpace::Kind::LinearSdr:
    case UI::UITargetSpace::Kind::Count: // size sentinel; no factory produces it
        break;
    }
    return 0;
}

UIManager::TextCoverageConstants UIManager::ResolveTextCoverageConstants(
    float contrast, float blendGamma, UI::UITargetSpace targetSpace)
{
    static_assert(UI::UITargetSpace::kKindCount == 5,
                  "A new UITargetSpace needs a blend-space arm here: a target that "
                  "interpolates raw sRGB-encoded bytes must resolve BlendSpaceEncoded = 1 "
                  "(the shader's Skia-direction retarget) — and its pins in "
                  "TextCoverageCorrectionTests must be updated with it.");
    // EncodedSrgb is the one attachment that interpolates raw sRGB-encoded
    // bytes; it selects the shader's Skia-direction retarget arm. Every other
    // space — LinearSdr and the three HDR flavours — is a linear-light
    // attachment: the ROP interpolates linear values and the terminal encode
    // owns the OETF. blendGamma names the TARGET space in both arms, so it
    // passes through un-gated: each arm is the identity retarget at its own
    // space (the linear arm at 0, the encoded arm at 1.0) and otherwise
    // solves for the coverage whose blend lands where the target space's
    // blend would.
    const int blendSpaceEncoded =
        targetSpace.GetKind() == UI::UITargetSpace::Kind::EncodedSrgb ? 1 : 0;
    return TextCoverageConstants{std::clamp(contrast, 0.0f, 1.0f),
                                 std::clamp(blendGamma, 0.0f, 4.0f), blendSpaceEncoded};
}

bool UIManager::RenderRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture target,
                         UI::UITargetSpace targetSpace)
{
    // Default arm: the UI owns the target and clears it (editor chrome path).
    return RenderRG(frame, target, targetSpace, RenderGraph::RGLoadOp::Clear);
}

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
struct UIManager::SdfRenderGraphPass
{
    struct ResolvedBinding
    {
        uint32_t Slot = 0;
        RenderGraph::RGTexture Texture{};
    };

    UIManager* Owner = nullptr;
    std::vector<ResolvedBinding> Bindings;
    BufferHandle PrimitiveBuffer{};
    BufferHandle ClipBuffer{};
    BufferHandle DrawOrderBuffer{};
    bool UploadsOk = false;
    int OutputEncoding = 0;
    bool BlendEncoded = false;
    TextCoverageConstants TextCoverage{};
    GraphicsPipelineId Pipeline{};

    void Declare(RenderGraph::RGFrame& frame, RenderGraph::RGPassBuilder& pass,
                 RenderGraph::RGTexture target, RenderGraph::RGLoadOp loadOp) const
    {
        // Clear when the UI owns the target; load when compositing over a scene.
        // The clear color is ignored under Load.
        RenderGraph::RGAttachmentOps ops{};
        ops.Load = loadOp;
        ops.Store = RenderGraph::RGStoreOp::Store;
        ops.Clear.Color[0] = 0.0f;
        ops.Clear.Color[1] = 0.0f;
        ops.Clear.Color[2] = 0.0f;
        ops.Clear.Color[3] = 1.0f;
        pass.AttachColor(0, target, ops);
        if (Pipeline.IsValid())
            pass.UsePipeline(Pipeline);

        // SSBO reads (ring buffers rotate per device frame; the import
        // dedups by physical so repeat declares are id-stable).
        if (PrimitiveBuffer.IsValid())
            pass.Read(frame.ImportExternalBuffer("UISdfPrims", PrimitiveBuffer),
                   RenderGraph::RGBufferRead::Storage);
        if (ClipBuffer.IsValid())
            pass.Read(frame.ImportExternalBuffer("UISdfClips", ClipBuffer),
                   RenderGraph::RGBufferRead::Storage);
        if (DrawOrderBuffer.IsValid())
            pass.Read(frame.ImportExternalBuffer("UISdfDrawOrder", DrawOrderBuffer),
                   RenderGraph::RGBufferRead::Storage);

        // Sampled reads order each published texture producer before UI sampling.
        for (const ResolvedBinding& binding : Bindings)
            pass.Read(binding.Texture, RenderGraph::RGTextureRead::Sampled);
    }

    void Record(RenderGraph::RGContext& context) const
    {
        UIManager& owner = *Owner;
        // Slot updates run even on draw-skipped frames: the published
        // producers rendered this frame, and the registry slots must
        // track the current physicals for the NEXT drawn frame.
        if (owner.m_SdfTextureRegistry)
        {
            for (const ResolvedBinding& binding : Bindings)
            {
                const TextureHandle physical = context.GetTexture(binding.Texture);
                if (physical.IsValid())
                    owner.m_SdfTextureRegistry->UpdateSlot(binding.Slot, physical);
            }
        }
        const size_t drawCount = owner.m_DrawOrder.size();
        if (!UploadsOk || drawCount == 0)
            return;
        const size_t totalSlots = owner.m_PrimitiveAllocator.GetTotalSlots();
        if (totalSlots == 0)
            return;

        const uint32_t frameIndex = owner.m_Device->GetFrameIndex();

        // Device metadata controls lift strength; the declared target space enables it.
        const auto hdrState = owner.m_Device->GetHdrOutputState();

        // An encoded target requires its staged encoded shader variant.
        if (BlendEncoded && !owner.m_SdfEncodedVariantAvailable)
            return;

        const PipelineHandle pipeline = context.GetOrCreatePipelineVariant(Pipeline);
        if (!pipeline.IsValid())
            return;
        context.Cmd->SetPipeline(pipeline);

        uint32_t width = owner.m_LastLayoutWidth;
        uint32_t height = owner.m_LastLayoutHeight;
        if (width == 0 || height == 0)
            owner.m_Device->GetSwapchainSize(width, height);

        context.Cmd->SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        context.Cmd->SetScissor(0, 0, width, height);

        // Push constants control linear-space compositing; the terminal pass encodes.
        SdfPushConstants pushConstants{};
        pushConstants.TargetSize[0] = static_cast<float>(width);
        pushConstants.TargetSize[1] = static_cast<float>(height);
        pushConstants.TimeSeconds = owner.m_Time;
        // Read the OS caret setting each frame so changes take effect immediately.
        pushConstants.CaretPhaseTogglesPerSecond =
            UI::CaretPhaseTogglesPerSecond(Platform::GetCaretBlinkHalfPeriod());
        pushConstants.TextContrast = TextCoverage.Contrast;
        pushConstants.TextBlendGamma = TextCoverage.BlendGamma;
        pushConstants.TextBlendSpaceEncoded = TextCoverage.BlendSpaceEncoded;
        pushConstants.EdgeSoftness = std::max(0.01f, owner.m_TextSmoothingGamma);
        pushConstants.OutputEncoding = OutputEncoding;
        const float deviceWhite =
            std::clamp(hdrState.staticMetadata.paperWhiteNits, kHdrPaperWhiteFloorNits, 1000.0f);
        pushConstants.PaperWhiteNits = owner.m_HdrUiPaperWhiteNits > 0.0f
            ? std::clamp(owner.m_HdrUiPaperWhiteNits, 40.0f, 350.0f)
            : deviceWhite;
        pushConstants.DeviceWhiteNits = deviceWhite;
        // Auto (< 0): in HDR modes, lift by the display's black floor so
        // dark chrome sits at the darkest level the panel can actually
        // show — effectively 0 on OLED, the backlight floor on LCD.
        pushConstants.BlackLiftNits = owner.m_HdrUiBlackLiftNits >= 0.0f
            ? owner.m_HdrUiBlackLiftNits
            : (pushConstants.OutputEncoding >= 2
                   ? std::clamp(hdrState.staticMetadata.minMasteringLuminance,
                                kHdrUiBlackLiftMinNits, kHdrUiBlackLiftMaxNits)
                   : 0.0f);
        if (!PrimitiveBuffer.IsValid() || !DrawOrderBuffer.IsValid())
            return;

        DescriptorSetDesc descriptorDesc{};
        descriptorDesc.layout = owner.m_SdfSsboSetLayout;
        descriptorDesc.debugName = "UISdf_SSBO_Set";
        descriptorDesc.transient = true;
        DescriptorSetHandle storageSet = owner.m_Device->CreateDescriptorSet(descriptorDesc);
        if (!storageSet.IsValid())
            return;

        const size_t primitiveBytes  = totalSlots * sizeof(UI::UIPrimitive);
        const size_t primitiveOffset = owner.m_SdfPrimRing->GetFrameOffset(frameIndex);
        owner.m_Device->UpdateStorageBufferBinding(storageSet, 0, PrimitiveBuffer, primitiveOffset, primitiveBytes);

        if (ClipBuffer.IsValid())
        {
            const size_t clipSlots = owner.m_ClipAllocator.GetTotalSlots();
            const size_t clipBytes = std::max(clipSlots, size_t(1)) * sizeof(UI::UIClipRect);
            const size_t clipOffset = owner.m_SdfClipRing->GetFrameOffset(frameIndex);
            owner.m_Device->UpdateStorageBufferBinding(storageSet, 1, ClipBuffer, clipOffset, clipBytes);
        }

        const size_t drawOrderBytes  = drawCount * sizeof(uint32_t);
        const size_t drawOrderOffset = owner.m_SdfDrawOrderRing->GetFrameOffset(frameIndex);
        owner.m_Device->UpdateStorageBufferBinding(storageSet, 2, DrawOrderBuffer, drawOrderOffset,
                                             drawOrderBytes);

        context.Cmd->BindDescriptorSet(0, storageSet, pipeline);

        // Each compatibility run binds its own textures and draw-order offset.
        // Consecutive runs preserve painter order.
        if (owner.m_SdfCompatProfile)
        {
            if (!owner.m_SdfTextureRegistry)
                return;
            for (const UI::UICompatDrawRun& run : owner.m_SdfCompatRuns.Runs())
            {
                if (run.Count == 0)
                    continue;
                const DescriptorSetHandle textureSet = owner.m_SdfTextureRegistry->CreateDrawRunSet(run);
                if (!textureSet.IsValid())
                    continue;
                context.Cmd->BindDescriptorSet(1, textureSet, pipeline);
                pushConstants.DrawBase = run.First;
                context.Cmd->SetConstants(0, sizeof(pushConstants), &pushConstants);
                context.Cmd->Draw(/*vertexCount=*/6u,
                              /*instanceCount=*/run.Count,
                              /*firstVertex=*/0u,
                              /*firstInstance=*/0u);
            }
            return;
        }

        context.Cmd->SetConstants(0, sizeof(pushConstants), &pushConstants);

        if (owner.m_SdfTextureRegistry)
        {
            DescriptorSetHandle textureSet = owner.m_SdfTextureRegistry->CreateTransientFrameSet();
            if (textureSet.IsValid())
                context.Cmd->BindDescriptorSet(1, textureSet, pipeline);
        }

        context.Cmd->Draw(/*vertexCount=*/6u,
                      /*instanceCount=*/static_cast<uint32_t>(drawCount),
                      /*firstVertex=*/0u,
                      /*firstInstance=*/0u);
    }
};
#endif

bool UIManager::RenderRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture target,
                         UI::UITargetSpace targetSpace, RenderGraph::RGLoadOp loadOp)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!target.IsValid() || !m_Device)
        return false;

    // The attachment clears even when preparation produces no drawable UI.
    const SdfFrameData frameData = PrepareSdfFrameData();

    // Resolve only textures published for this frame incarnation.
    // Unresolved slots retain their last physical texture.
    std::vector<SdfRenderGraphPass::ResolvedBinding> resolvedBindings;
    resolvedBindings.reserve(m_SdfDeferredRGBindings.size());
    for (const auto& binding : m_SdfDeferredRGBindings)
    {
        if (binding.ExternalName.empty())
            continue; // No frame publication names this binding.
        const auto publication = m_ExternalRGPublished.find(binding.ExternalName);
        if (publication == m_ExternalRGPublished.end())
            continue;
        const PublishedRGTexture& published = publication->second;
        if (!published.For.IsFor(frame) || published.TexId == RenderGraph::kInvalidId)
            continue; // stale publish — never consume a foreign frame's id
        resolvedBindings.push_back({binding.Slot, RenderGraph::RGTexture{published.TexId}});
    }

    const BufferHandle primitiveBuffer      = m_SdfPrimRing ? m_SdfPrimRing->GetBuffer() : BufferHandle{};
    const BufferHandle clipBuffer      = m_SdfClipRing ? m_SdfClipRing->GetBuffer() : BufferHandle{};
    const BufferHandle drawOrderBuffer = m_SdfDrawOrderRing ? m_SdfDrawOrderRing->GetBuffer() : BufferHandle{};

    const bool uploadsOk = frameData.UploadsOk;

    // The declared target space controls encoding and subpixel activation.
    const int outputEncoding = ResolveOutputEncoding(targetSpace);

    // Encoded targets require the matching shader variant.
    // A missing variant skips draws while preserving the attachment clear.
    const bool blendEncoded =
        targetSpace.GetKind() == UI::UITargetSpace::Kind::EncodedSrgb;
#if defined(GE_DEV_DIAG)
    if (blendEncoded && !m_SdfEncodedVariantAvailable)
    {
        static bool s_WarnedEncodedVariantMissing = false;
        if (!s_WarnedEncodedVariantMissing)
        {
            s_WarnedEncodedVariantMissing = true;
            Logger::Log::Warning(
                "[UI] target declared EncodedSrgb but the encoded shader variant is not "
                "staged — UI draws are skipped (clear-only frames)");
        }
    }
#endif

    // Unsupported subpixel rendering uses the matching grayscale variant.
    const bool subpixelActive = ResolveTextSubpixelActive(
        m_TextSubpixelAA, outputEncoding,
        m_Device->GetCapabilities().supportsDualSourceBlending,
        blendEncoded ? m_SdfSubpixelEncodedVariantAvailable : m_SdfSubpixelVariantAvailable);
    m_LastResolvedOutputEncoding = outputEncoding;
    m_LastTextSubpixelActive = subpixelActive;

    // Text-correction constants resolve at declare time with the rest of the
    // target-space translation: the attachment's blend space selects the
    // retarget arm, in this one place.
    const TextCoverageConstants textCoverage =
        ResolveTextCoverageConstants(m_TextContrast, m_TextBlendGamma, targetSpace);

    Rendering::GraphicsPipelineId pipelineId{};
    if (uploadsOk && !m_DrawOrder.empty() && m_PrimitiveAllocator.GetTotalSlots() != 0 &&
        (!blendEncoded || m_SdfEncodedVariantAvailable))
    {
        Rendering::GraphicsPipelineId& cachedPipelineId =
            blendEncoded
                ? (subpixelActive ? m_SdfPipelineIdSubpixelEncoded : m_SdfPipelineIdEncoded)
                : (subpixelActive ? m_SdfPipelineIdSubpixel : m_SdfPipelineId);
        if (!cachedPipelineId.IsValid())
        {
            Rendering::PipelineDesc& pipelineDesc =
                blendEncoded ? (subpixelActive ? m_SdfPipelineDescSubpixelEncoded
                                               : m_SdfPipelineDescEncoded)
                             : (subpixelActive ? m_SdfPipelineDescSubpixel : m_SdfPipelineDesc);
            pipelineDesc.debugName =
                blendEncoded ? (subpixelActive ? "UI_SDF_Subpixel_Encoded" : "UI_SDF_Encoded")
                             : (subpixelActive ? "UI_SDF_Subpixel" : "UI_SDF");
            cachedPipelineId =
                Rendering::PipelineDescTranslator::InternGraphics(*m_Device, pipelineDesc);
        }
        pipelineId = cachedPipelineId;
    }

    auto* data = frame.AllocatePassData<SdfRenderGraphPass>();
    data->Owner = this;
    data->Bindings = resolvedBindings;
    data->PrimitiveBuffer = primitiveBuffer;
    data->ClipBuffer = clipBuffer;
    data->DrawOrderBuffer = drawOrderBuffer;
    data->UploadsOk = uploadsOk;
    data->OutputEncoding = outputEncoding;
    data->BlendEncoded = blendEncoded;
    data->TextCoverage = textCoverage;
    data->Pipeline = pipelineId;

    frame.AddPass(
        "UI Overlay", static_cast<int32_t>(PassPhase::kUI),
        [data, &frame, target, loadOp](RenderGraph::RGPassBuilder& pass)
        {
            data->Declare(frame, pass, target, loadOp);
        },
        [data](RenderGraph::RGContext& context) { data->Record(context); });
    return true;
#else
    (void)frame;
    (void)target;
    (void)targetSpace;
    return false;
#endif
}
