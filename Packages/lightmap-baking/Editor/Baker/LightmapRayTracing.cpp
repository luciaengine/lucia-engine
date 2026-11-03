#include "Baker/ShadowLightmapBaker.h"
#include "Rendering/Core/AccelerationStructure.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Logger/Logger.h"
#include "Assets/ShaderProgramAsset.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>

namespace GameEngine::Engine::Renderer
{
namespace
{
using namespace GameEngine::Rendering;
constexpr size_t kRayCapacity=65536;
static_assert(sizeof(LightmapRay)==32 && offsetof(LightmapRay,Direction)==16);
static_assert(sizeof(LightmapRayHit)==16 && offsetof(LightmapRayHit,Triangle)==12);

// Kept with the packed host structures: this small kernel has no asset or shader
// include dependencies, and can also initialize in headless tools/tests.
constexpr const char* kIntersectionShader=R"GLSL(
#version 460
#extension GL_EXT_ray_query : require
layout(local_size_x=64) in;
struct Ray { vec3 origin; float maxDistance; vec3 direction; float minDistance; };
struct Hit { float distance; float u; float v; uint triangle; };
layout(set=0,binding=0) uniform accelerationStructureEXT uTlas;
layout(std430,set=0,binding=1) readonly buffer Rays { Ray rays[]; };
layout(std430,set=0,binding=2) writeonly buffer Hits { Hit hits[]; };
layout(std140,set=0,binding=3) uniform Params { uint count; };
void main()
{
    uint i=gl_GlobalInvocationID.x;
    if (i>=count) return;
    Ray bakeRay=rays[i];
    rayQueryEXT query;
    rayQueryInitializeEXT(query,uTlas,gl_RayFlagsOpaqueEXT,0xFFu,
        bakeRay.origin,bakeRay.minDistance,bakeRay.direction,bakeRay.maxDistance);
    while (rayQueryProceedEXT(query)) { }
    Hit hit=Hit(0,0,0,0xFFFFFFFFu);
    if (rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionTriangleEXT)
    {
        vec2 bary=rayQueryGetIntersectionBarycentricsEXT(query,true);
        hit=Hit(rayQueryGetIntersectionTEXT(query,true),bary.x,bary.y,
            rayQueryGetIntersectionPrimitiveIndexEXT(query,true));
    }
    hits[i]=hit;
}
)GLSL";

class GpuRayTracer
{
public:
    ~GpuRayTracer()
    {
        // The device belongs exclusively to this worker. Shutdown drains its
        // submissions before freeing descriptors, BLAS/TLAS and buffers, even
        // after a failed wait; no editor-renderer resources are touched.
        if (m_Device) m_Device->Shutdown();
    }
    bool Initialize(std::span<const float> positions)
    {
        if (positions.empty() || positions.size()%9 || positions.size()/3>UINT32_MAX) return false;
        m_TriangleCount=uint32_t(positions.size()/9);
        DeviceDesc desc{};
        desc.applicationName="Lightmap Baker";
        desc.enableSwapchain=false;
        m_Device=DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc) || !m_Device->GetCapabilities().supportsRayQuery) return false;
        auto* acceleration=m_Device->GetAccelerationStructureBackend();
        if (!acceleration || !m_Device->BeginFrame()) return false;
        acceleration->CollectGarbage();
        m_Timeline=m_Device->CreateTimelineSemaphore();
        if (!m_Timeline.IsValid()) return false;
        const auto inputUsage=BufferUsage::AccelerationStructureBuildInput|BufferUsage::ShaderDeviceAddress;
        const auto vertices=CreateBuffer(positions.size_bytes(),inputUsage,BufferMemoryUsage::Upload,"Lightmap vertices");
        std::vector<uint32_t> indices(positions.size()/3);
        std::iota(indices.begin(),indices.end(),0u);
        const auto indexBuffer=CreateBuffer(indices.size()*sizeof(uint32_t),inputUsage,BufferMemoryUsage::Upload,"Lightmap indices");
        const auto instances=CreateBuffer(sizeof(TlasInstanceData),inputUsage,BufferMemoryUsage::Upload,"Lightmap instances");
        m_Rays=CreateBuffer(kRayCapacity*sizeof(LightmapRay),BufferUsage::Storage,BufferMemoryUsage::Upload,"Lightmap rays");
        m_Hits=CreateBuffer(kRayCapacity*sizeof(LightmapRayHit),BufferUsage::Storage|BufferUsage::TransferSrc,BufferMemoryUsage::DeviceLocal,"Lightmap hits");
        m_Readback=m_Device->CreateReadbackBuffer(kRayCapacity*sizeof(LightmapRayHit),"Lightmap ray readback");
        m_Params=CreateBuffer(16,BufferUsage::Uniform,BufferMemoryUsage::Upload,"Lightmap ray count");
        if (!vertices.IsValid() || !indexBuffer.IsValid() || !instances.IsValid() || !m_Rays.IsValid() ||
            !m_Hits.IsValid() || !m_Readback.IsValid() || !m_Params.IsValid()) return false;
        m_Device->UpdateBuffer(vertices,0,positions.size_bytes(),positions.data());
        m_Device->UpdateBuffer(indexBuffer,0,indices.size()*sizeof(uint32_t),indices.data());
        BlasTriangleGeometry geometry{};
        geometry.VertexAddress=m_Device->GetBufferDeviceAddress(vertices);
        geometry.VertexStrideBytes=3*sizeof(float);
        geometry.MaxVertex=uint32_t(indices.size()-1);
        geometry.IndexAddress=m_Device->GetBufferDeviceAddress(indexBuffer);
        geometry.IndexCount=uint32_t(indices.size());
        geometry.IndexKind=IndexType::Uint32;
        if (!geometry.VertexAddress || !geometry.IndexAddress) return false;
        const auto blas=acceleration->CreateBlas(geometry);
        if (!blas.IsValid() || !acceleration->ReserveBlasScratch(blas)) return false;
        m_Tlas=acceleration->AcquireTlasSlot("Lightmap bake");
        if (!m_Tlas.IsValid() || !acceleration->PrepareTlas(m_Tlas,1)) return false;
        TlasInstanceData instance{};
        instance.Transform[0]=instance.Transform[5]=instance.Transform[10]=1;
        instance.CustomIndexAndMask=0xFF000000u;
        instance.SbtOffsetAndFlags=kTlasInstanceFlagTriangleFacingCullDisable<<24u;
        instance.BlasAddress=acceleration->GetBlasDeviceAddress(blas);
        if (!instance.BlasAddress) return false;
        m_Device->UpdateBuffer(instances,0,sizeof(instance),&instance);
        auto command=m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        if (!command) return false;
        command->Begin();
        acceleration->RecordPreBuildBarrier(*command);
        acceleration->RecordBlasBuild(*command,blas,geometry);
        acceleration->RecordTlasBuild(*command,m_Tlas,instances,0,1);
        command->End();
        if (!SubmitAndWait(*command)) return false;
        m_Device->FinalizeFrame();

        ShaderProgramCompileRequest request{};
        request.debugName="Lightmap GPU intersections";
        request.cacheRoot=ShaderProgramAsset::GetShaderCacheRoot();
        if (request.cacheRoot.empty()) return false;
        ShaderStageCompileSpec stage{};
        stage.stage="cs";
        stage.sourcePath="lightmap_intersections.comp";
        stage.inlineSource=kIntersectionShader;
        request.stages.push_back(std::move(stage));
        ShaderProgramCompileResult compiled{};
        std::string compileError;
        if (!ShaderCompileService::CompileProgramToCache(request,ShaderSourceKind::SpirV,compiled,&compileError))
        {
            Logger::Log::Warning("Lightmap GPU shader unavailable: {}",compileError);
            return false;
        }
        DescriptorSetLayoutDesc layout{};
        for (uint32_t i=0; i<4; ++i)
        {
            DescriptorBinding binding{};
            binding.binding=i; binding.count=1; binding.shaderStages=kShaderStageCompute;
            binding.type=i==0 ? DescriptorType::AccelerationStructure : (i==3 ? DescriptorType::UniformBuffer : DescriptorType::StorageBuffer);
            layout.bindings.push_back(binding);
        }
        PipelineDesc pipeline{};
        pipeline.type=PipelineType::Compute;
        pipeline.debugName="Lightmap GPU intersections";
        pipeline.computeShader=std::move(compiled.stageBytes.at("cs"));
        pipeline.descriptorSetLayouts={layout};
        m_Pipeline=m_Device->CreatePipeline(pipeline);
        if (!m_Pipeline.IsValid()) return false;
        DescriptorSetDesc descriptor{};
        descriptor.layout=layout; descriptor.debugName="Lightmap ray query descriptors";
        m_Set=m_Device->CreateDescriptorSet(descriptor);
        if (!m_Set.IsValid()) return false;
        DescriptorSetUpdate tlas{};
        tlas.type=DescriptorType::AccelerationStructure; tlas.accelerationStructures={m_Tlas};
        m_Device->UpdateDescriptorSet(m_Set,tlas);
        m_Device->UpdateStorageBufferBinding(m_Set,1,m_Rays,0,kRayCapacity*sizeof(LightmapRay));
        m_Device->UpdateStorageBufferBinding(m_Set,2,m_Hits,0,kRayCapacity*sizeof(LightmapRayHit));
        m_Device->UpdateBufferBinding(m_Set,3,m_Params,0,16);
        Logger::Log::Info("Lightmap baker: GPU ray queries enabled ({} triangles)",m_TriangleCount);
        return true;
    }
    bool Trace(std::span<const LightmapRay> rays, std::span<LightmapRayHit> hits)
    {
        if (m_Failed || rays.size()>kRayCapacity || hits.size()!=rays.size()) return false;
        if (rays.empty()) return true;
        if (!m_Device->BeginFrame()) return Fail();
        const uint32_t count[4]={uint32_t(rays.size()),0,0,0};
        m_Device->UpdateBuffer(m_Params,0,sizeof(count),count);
        m_Device->UpdateBuffer(m_Rays,0,rays.size_bytes(),rays.data());
        auto command=m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        if (!command) return Fail();
        command->Begin();
        command->Barrier(ResourceBarrier::CreateBufferBarrier(m_Hits,m_HasTraced ? ResourceState::CopySource : ResourceState::Undefined,ResourceState::UnorderedAccess));
        command->SetPipeline(m_Pipeline);
        command->BindDescriptorSet(0,m_Set,m_Pipeline);
        command->Dispatch((uint32_t(rays.size())+63u)/64u,1,1);
        command->Barrier(ResourceBarrier::CreateBufferBarrier(m_Hits,ResourceState::UnorderedAccess,ResourceState::CopySource));
        command->CopyBuffer(m_Hits,m_Readback,hits.size_bytes());
        auto hostRead=ResourceBarrier::CreateBufferBarrier(m_Readback,ResourceState::CopyDest,ResourceState::Common);
        // Public barrier masks use Vulkan synchronization bits; Metal ends the
        // encoder and uses tracked-resource hazards. Make transfer writes visible
        // to host reads before the timeline signal, then invalidate on MapBuffer.
        hostRead.srcStageMask=0x1000; hostRead.srcAccessMask=0x1000; // transfer / transfer write
        hostRead.dstStageMask=0x4000; hostRead.dstAccessMask=0x2000; // host / host read
        command->Barrier(hostRead);
        command->End();
        if (!SubmitAndWait(*command)) return Fail();
        m_Device->FinalizeFrame();
        m_HasTraced=true;
        const auto* mapped=m_Device->MapBuffer(m_Readback);
        if (!mapped) return Fail();
        std::memcpy(hits.data(),mapped,hits.size_bytes());
        m_Device->UnmapBuffer(m_Readback);
        for (size_t i=0; i<hits.size(); ++i)
        {
            const auto& hit=hits[i];
            if (hit.Triangle==UINT32_MAX) continue;
            if (hit.Triangle>=m_TriangleCount || !std::isfinite(hit.Distance) || !std::isfinite(hit.U) || !std::isfinite(hit.V) ||
                hit.Distance<rays[i].MinDistance || hit.Distance>rays[i].MaxDistance || hit.U<0 || hit.V<0 || hit.U+hit.V>1.00001f)
                return Fail();
        }
        return true;
    }
private:
    BufferHandle CreateBuffer(size_t size, BufferUsage usage, BufferMemoryUsage memory, const char* name)
    {
        BufferDesc desc{};
        desc.size=size; desc.usage=uint32_t(usage); desc.memoryUsage=memory; desc.debugName=name;
        return m_Device->CreateBuffer(desc);
    }
    bool SubmitAndWait(CommandList& command)
    {
        const auto value=++m_Submission;
        return m_Device->QueueSubmit(IDevice::QueueType::Graphics,{&command},{},{{m_Timeline,value}}) &&
            m_Device->WaitTimelineSemaphoreValue(m_Timeline,value,10'000'000'000ull);
    }
    bool Fail()
    {
        m_Failed=true;
        Logger::Log::Warning("Lightmap GPU ray batch failed; continuing on CPU");
        return false;
    }
    std::unique_ptr<IDevice> m_Device;
    BufferHandle m_Rays{},m_Hits{},m_Readback{},m_Params{};
    PipelineHandle m_Pipeline{};
    DescriptorSetHandle m_Set{};
    SemaphoreHandle m_Timeline{};
    TlasSlotHandle m_Tlas{};
    uint64_t m_Submission=0;
    uint32_t m_TriangleCount=0;
    bool m_HasTraced=false,m_Failed=false;
};
}
LightmapRayBatch CreateGpuLightmapRayTracer(std::span<const float> positions)
{
    try
    {
        auto tracer=std::make_shared<GpuRayTracer>();
        if (tracer->Initialize(positions))
            return [tracer](std::span<const LightmapRay> rays,std::span<LightmapRayHit> hits) {
                try { return tracer->Trace(rays,hits); }
                catch (const std::exception& e)
                { Logger::Log::Warning("Lightmap GPU tracing failed: {}",e.what()); return false; }
            };
    }
    catch (const std::exception& e)
    { Logger::Log::Warning("Lightmap GPU initialization failed: {}",e.what()); }
    Logger::Log::Info("Lightmap baker: CPU ray tracing selected (GPU ray queries unavailable)");
    return {};
}
}
