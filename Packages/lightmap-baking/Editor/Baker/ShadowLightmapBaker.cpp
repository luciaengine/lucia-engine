// Directional shadowmask approach adapted from Godot lightmapper_rd/lm_compute.glsl.
// Runtime blend equations mirror scene_forward_clustered.glsl.
// See Packages/lightmap-baking/Licenses/Godot-MIT.txt for attribution and license.
#include "Baker/ShadowLightmapBaker.h"
#include "Assets/ModelAsset.h"
#include "SceneBvh/ThreadedBvhBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace GameEngine::Engine::Renderer
{
namespace
{
bool Finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
struct Triangle { glm::vec3 P[3], N[3], Color[3]; glm::vec2 UV[3]; glm::vec3 Face; uint32_t Vertex[3]; };
float Luminance(glm::vec3 v) { return glm::dot(v,glm::vec3(0.2126f,0.7152f,0.0722f)); }
float RadicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits >> 8u) * (1.0f / 16777216.0f);
}

struct DenoiseGuide
{
    glm::vec3 Position{0}, Normal{0};
    float Footprint = 0;
    uint32_t Chart = 0;
};

// Vertices already carry one UV1 coordinate. Shared index edges connect a chart;
// duplicated vertices at unwrap seams deliberately keep the charts separate.
std::vector<uint32_t> FindCharts(const std::vector<Triangle>& triangles)
{
    std::vector<uint32_t> parent(triangles.size());
    std::iota(parent.begin(),parent.end(),0u);
    auto root = [&](uint32_t i) {
        while (parent[i]!=i) { parent[i]=parent[parent[i]]; i=parent[i]; }
        return i;
    };
    std::unordered_map<uint64_t,uint32_t> edges;
    for (uint32_t i=0; i<triangles.size(); ++i)
        for (uint32_t e=0; e<3; ++e)
        {
            const auto a=triangles[i].Vertex[e], b=triangles[i].Vertex[(e+1)%3];
            const uint64_t key=(uint64_t(std::min(a,b))<<32u)|std::max(a,b);
            const auto [it,inserted]=edges.emplace(key,i);
            if (!inserted) parent[root(i)]=root(it->second);
        }
    for (auto& i : parent) i=root(i);
    return parent;
}

bool DenoiseDiffuse(ShadowLightmapBakeResult& result, const std::vector<uint8_t>& coverage,
                    const std::vector<DenoiseGuide>& guides, std::vector<float> variance,
                    const std::function<bool()>& keepGoing, const std::function<void(float)>& progress)
{
    // A-trous filtering in irradiance space (before material albedo and before
    // dilation). Sample variance controls colour rejection; geometry prevents
    // light from bleeding across folds, unrelated charts or empty atlas texels.
    constexpr float kernel[5]={1,4,6,4,1};
    auto rgb = [&](size_t i) { return glm::vec3(result.IndirectRGB[i*3],result.IndirectRGB[i*3+1],result.IndirectRGB[i*3+2]); };
    for (int pass=0; pass<3; ++pass)
    {
        const int step=1<<pass;
        auto next=result.IndirectRGB;
        auto nextVariance=variance;
        for (uint32_t y=0; y<result.Height; ++y)
        {
            if (keepGoing && !keepGoing()) return false;
            if (progress) progress(0.9f + 0.08f*(pass+float(y)/result.Height)/3.0f);
            for (uint32_t x=0; x<result.Width; ++x)
            {
                const size_t i=size_t(y)*result.Width+x;
                if (!coverage[i]) continue;
                const auto& a=guides[i];
                if (a.Chart==UINT32_MAX) continue;
                const auto center=rgb(i);
                glm::vec3 sum(0);
                float weightSum=0, varianceSum=0;
                for (int ky=-2; ky<=2; ++ky)
                for (int kx=-2; kx<=2; ++kx)
                {
                    const int nx=int(x)+kx*step, ny=int(y)+ky*step;
                    if (nx<0 || ny<0 || nx>=int(result.Width) || ny>=int(result.Height)) continue;
                    const size_t j=size_t(ny)*result.Width+nx;
                    const auto& b=guides[j];
                    if (!coverage[j] || a.Chart!=b.Chart) continue;
                    const float alignment=std::max(0.0f,glm::dot(a.Normal,b.Normal));
                    if (alignment<0.9f) continue;
                    const auto delta=b.Position-a.Position;
                    const float scale=std::max(1e-6f,std::max(a.Footprint,b.Footprint)*step);
                    if (glm::dot(delta,delta)>scale*scale*18) continue;
                    const float plane=std::max(std::abs(glm::dot(delta,a.Normal)),std::abs(glm::dot(delta,b.Normal)));
                    const auto value=rgb(j);
                    const float sigma=4*std::sqrt(variance[i]+variance[j])+0.02f*std::max(Luminance(center),Luminance(value))+1e-5f;
                    const float colourDistance=Luminance(glm::abs(center-value));
                    const float weight=kernel[kx+2]*kernel[ky+2]*std::pow(alignment,32.0f)*
                        std::exp(-colourDistance/sigma-plane/(scale*0.25f));
                    sum+=value*weight; weightSum+=weight; varianceSum+=variance[j]*weight*weight;
                }
                if (weightSum>0)
                {
                    sum/=weightSum;
                    for (int c=0; c<3; ++c) next[i*3+c]=sum[c];
                    nextVariance[i]=varianceSum/(weightSum*weightSum);
                }
            }
        }
        result.IndirectRGB.swap(next); variance.swap(nextVariance);
    }
    return true;
}

bool Gather(const ShadowLightmapMesh& source, bool receiver, std::vector<Triangle>& triangles, std::string& error)
{
    const auto* mesh = source.Geometry;
    auto fail = [&](const char* message) { error = message; return false; };
    if (!mesh || mesh->PrimitiveTopology != MeshPrimitiveTopology::Triangles || mesh->Vertices.empty() || mesh->IsSkinned())
        return fail("Shadow lightmaps require static triangle meshes.");
    if (receiver && !mesh->HasTexCoords1())
        return fail("Receiver needs a non-overlapping UV1 unwrap (second UV channel).");
    const auto& m = source.LocalToWorld;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(m[c][r])) return fail("Non-finite mesh transform.");
    if (m[0][3] != 0 || m[1][3] != 0 || m[2][3] != 0 || m[3][3] != 1 ||
        std::abs(glm::determinant(glm::mat3(m))) < 1e-12f)
        return fail("Mesh transform must be affine and invertible.");
    const auto normalMatrix = glm::transpose(glm::inverse(glm::mat3(m)));
    const size_t count = mesh->Indices.empty() ? mesh->Vertices.size() : mesh->Indices.size();
    if (count % 3 != 0) return fail("Triangle index count must be divisible by three.");
    for (size_t t = 0; t < count; t += 3)
    {
        Triangle tri{};
        for (size_t j = 0; j < 3; ++j)
        {
            const size_t i = mesh->Indices.empty() ? t + j : mesh->Indices[t + j];
            if (i >= mesh->Vertices.size()) return fail("Triangle index outside the vertex array.");
            tri.Vertex[j]=uint32_t(i);
            const auto& v = mesh->Vertices[i];
            tri.P[j] = glm::vec3(m * glm::vec4(v.Position[0], v.Position[1], v.Position[2], 1));
            if (!Finite(tri.P[j])) return fail("Non-finite world-space vertex.");
            tri.N[j] = normalMatrix * glm::vec3(v.Normal[0],v.Normal[1],v.Normal[2]);
            if (!Finite(tri.N[j])) return fail("Non-finite vertex normal.");
            tri.Color[j] = mesh->HasColor0() && source.UseVertexColor ?
                glm::vec3(mesh->Color0[i*4],mesh->Color0[i*4+1],mesh->Color0[i*4+2]) : glm::vec3(1);
            if (!Finite(tri.Color[j])) return fail("Non-finite vertex colour.");
            if (!receiver) tri.UV[j] = {v.TexCoords[0], v.TexCoords[1]};
            if (receiver)
            {
                tri.UV[j] = {mesh->TexCoords1[i * 2], mesh->TexCoords1[i * 2 + 1]};
                if (!std::isfinite(tri.UV[j].x) || !std::isfinite(tri.UV[j].y) ||
                    tri.UV[j].x < 0 || tri.UV[j].x > 1 || tri.UV[j].y < 0 || tri.UV[j].y > 1)
                    return fail("Receiver UV1 must be finite and inside [0,1].");
            }
        }
        const auto cross = glm::cross(tri.P[1] - tri.P[0], tri.P[2] - tri.P[0]);
        if (glm::length(cross) > 1e-12f)
        {
            // Renderer normals use inverse transpose, including mirrored instances.
            tri.Face = glm::normalize(cross) * (glm::determinant(glm::mat3(m)) < 0 ? -1.0f : 1.0f);
            for (auto& n : tri.N) n = glm::length(n)>1e-8f ? glm::normalize(n) : tri.Face;
            triangles.push_back(tri);
        }
    }
    return true;
}

struct Hit { float Distance = std::numeric_limits<float>::max(); uint32_t Triangle = 0; glm::vec3 Bary{0}; };
bool Trace(const SceneBvh::ThreadedBvh& bvh, glm::vec3 origin, glm::vec3 direction, Hit* closest = nullptr, float minDistance = 0)
{
    bool found = false;
    uint32_t cursor = 0;
    while (cursor < bvh.NodeCount())
    {
        const auto box = SceneBvh::DecodeNodeBounds(bvh.Nodes.data(), cursor);
        const float lo[3] = {box.min.x, box.min.y, box.min.z};
        const float hi[3] = {box.max.x, box.max.y, box.max.z};
        float near = 0, far = closest ? closest->Distance : std::numeric_limits<float>::max();
        bool hit = true;
        for (int a = 0; a < 3; ++a)
        {
            if (std::abs(direction[a]) < 1e-20f)
            {
                if (origin[a] < lo[a] || origin[a] > hi[a]) hit = false;
            }
            else
            {
                const float t0 = (lo[a] - origin[a]) / direction[a];
                const float t1 = (hi[a] - origin[a]) / direction[a];
                near = std::max(near, std::min(t0, t1));
                far = std::min(far, std::max(t0, t1));
            }
        }
        const auto miss = SceneBvh::DecodeNodeMissLink(bvh.Nodes.data(), cursor);
        if (!hit || far < near) { cursor = miss; continue; }
        const auto leaf = SceneBvh::DecodeNodeLeafWord(bvh.Nodes.data(), cursor);
        if (SceneBvh::IsInteriorLeafWord(leaf)) { ++cursor; continue; }
        const auto begin = SceneBvh::DecodeLeafTriangleOffset(leaf);
        for (uint32_t t = begin; t < begin + SceneBvh::DecodeLeafTriangleCount(leaf); ++t)
        {
            glm::vec3 p[3];
            for (int j = 0; j < 3; ++j)
            {
                const auto i = bvh.TriangleIndices[t * 3 + j] * SceneBvh::kVertexDataStrideFloats;
                p[j] = {bvh.VertexData[i], bvh.VertexData[i + 1], bvh.VertexData[i + 2]};
            }
            const auto e1 = p[1] - p[0], e2 = p[2] - p[0];
            const auto h = glm::cross(direction, e2);
            const double det = glm::dot(e1, h);
            if (std::abs(det) < 1e-12) continue;
            const auto s = origin - p[0];
            const double u = glm::dot(s, h) / det;
            if (u < 0 || u > 1) continue;
            const auto q = glm::cross(s, e1);
            const double v = glm::dot(direction, q) / det;
            if (v < 0 || u + v > 1) continue;
            const float distance = float(glm::dot(e2, q) / det);
            if (distance >= minDistance && (!closest || distance < closest->Distance))
            {
                if (!closest) return true;
                closest->Distance = distance;
                closest->Triangle = bvh.TriangleMaterials[t];
                closest->Bary = glm::vec3(1-u-v,u,v);
                found = true;
            }
        }
        cursor = miss;
    }
    return found;
}
glm::vec3 Hemisphere(glm::vec3 n, float radial, float azimuth)
{
    const float r=std::sqrt(radial), phi=6.28318530718f*azimuth;
    const auto t=glm::normalize(glm::cross(std::abs(n.y)<0.9f ? glm::vec3(0,1,0) : glm::vec3(1,0,0),n));
    return glm::normalize(t*(r*std::cos(phi))+glm::cross(n,t)*(r*std::sin(phi))+
        n*std::sqrt(std::max(0.0f,1-r*r)));
}
struct BakeSample
{
    size_t Pixel;
    uint32_t Seed;
    glm::vec3 Position, Face, Normal;
};

// Wavefront tracing batches many independent paths at each bounce. Only the
// intersection operation changes backend; textures, lights and filtering retain
// the same CPU evaluation, including user-supplied material/sky callbacks.
bool IntegrateSamples(std::span<const BakeSample> samples, const std::vector<Triangle>& casters,
    const std::vector<size_t>& materials, std::span<const ShadowLightmapMesh> occluders,
    const ShadowLightmapBakeOptions& options, const LightmapRayBatch& trace,
    ShadowLightmapBakeResult& result, std::vector<float>& sums, std::vector<double>& moments,
    std::string& error)
{
    const auto light=glm::normalize(options.DirectionToLight);
    const auto tangent=glm::normalize(glm::cross(std::abs(light.y)<0.9f ? glm::vec3(0,1,0) : glm::vec3(1,0,0),light));
    const auto bitangent=glm::cross(light,tangent);
    const uint32_t shadowSamples=options.AngularRadiusRadians==0 ? 1 : options.RaysPerSample;
    std::vector<LightmapRay> rays;
    std::vector<LightmapRayHit> hits;
    auto intersect=[&]() {
        hits.resize(rays.size());
        if (!trace(rays,hits)) { error="Shadow lightmap bake cancelled."; return false; }
        return true;
    };
    for (const auto& s : samples)
    {
        const auto origin=s.Position+(glm::dot(s.Face,light)<0 ? -s.Face : s.Face)*options.Bias;
        const float rotation=float(s.Seed&65535u)*(6.28318530718f/65536.0f);
        for (uint32_t r=0; r<shadowSamples; ++r)
        {
            const float radius=std::tan(options.AngularRadiusRadians)*std::sqrt((r+0.5f)/shadowSamples);
            const float angle=r*2.39996322973f+rotation;
            rays.push_back({origin,std::numeric_limits<float>::max(),
                glm::normalize(light+radius*(tangent*std::cos(angle)+bitangent*std::sin(angle)))});
        }
    }
    if (!intersect()) return false;
    for (size_t i=0; i<samples.size(); ++i)
    {
        uint32_t lit=0;
        for (uint32_t r=0; r<shadowSamples; ++r) lit+=hits[i*shadowSamples+r].Triangle==UINT32_MAX;
        sums[samples[i].Pixel]+=float(lit)/shadowSamples;
    }
    if (!options.IndirectSamples) return true;
    struct Path
    {
        glm::vec3 Origin, Direction, Throughput{1}, Radiance{0};
        uint32_t Seed;
        bool Active=true;
    };
    std::vector<Path> paths;
    paths.reserve(samples.size()*options.IndirectSamples);
    for (const auto& s : samples)
        for (uint32_t p=0; p<options.IndirectSamples; ++p)
        {
            const float shift=float(s.Seed>>8u)*(1.0f/16777216.0f);
            Path path;
            path.Origin=s.Position+s.Face*options.Bias;
            path.Direction=Hemisphere(s.Normal,glm::fract((p+0.5f)/options.IndirectSamples+shift),RadicalInverse(p^s.Seed));
            // Per-path streams make secondary sampling independent of batch
            // size, scheduling, and the selected intersection backend.
            path.Seed=s.Seed^(p*747796405u+2891336453u);
            path.Active=glm::dot(path.Direction,s.Face)>0;
            paths.push_back(path);
        }
    std::vector<size_t> active, visibilityPaths;
    std::vector<glm::vec3> contributions;
    std::vector<LightmapRay> visibilityRays;
    for (uint32_t bounce=0; bounce<options.IndirectBounces; ++bounce)
    {
        rays.clear(); active.clear();
        for (size_t i=0; i<paths.size(); ++i)
            if (paths[i].Active)
            {
                active.push_back(i);
                rays.push_back({paths[i].Origin,std::numeric_limits<float>::max(),paths[i].Direction});
            }
        if (rays.empty()) break;
        if (!intersect()) return false;
        visibilityRays.clear(); visibilityPaths.clear(); contributions.clear();
        // Bound shadow-ray memory even when the scene contains many lights.
        // Flush separately from primary hit storage, which shading still reads.
        auto flushVisibility=[&]() {
            std::vector<LightmapRayHit> blockers(visibilityRays.size());
            if (!trace(visibilityRays,blockers)) { error="Shadow lightmap bake cancelled."; return false; }
            for (size_t i=0; i<blockers.size(); ++i)
                if (blockers[i].Triangle==UINT32_MAX) paths[visibilityPaths[i]].Radiance+=contributions[i];
            visibilityRays.clear(); visibilityPaths.clear(); contributions.clear();
            return true;
        };
        for (size_t i=0; i<active.size(); ++i)
        {
            if ((i&255u)==0 && options.Continue && !options.Continue())
            { error="Shadow lightmap bake cancelled."; return false; }
            auto& path=paths[active[i]];
            const auto& hit=hits[i];
            if (hit.Triangle==UINT32_MAX)
            {
                const auto environment=options.SampleEnvironment ? options.SampleEnvironment(path.Direction) : options.EnvironmentRadiance;
                if (!Finite(environment) || glm::any(glm::lessThan(environment,glm::vec3(0))))
                { error="Environment sampler returned invalid radiance."; return false; }
                path.Radiance+=path.Throughput*environment; path.Active=false;
                continue;
            }
            const auto& triangle=casters[hit.Triangle];
            const auto& material=occluders[materials[hit.Triangle]];
            const glm::vec3 bary(1-hit.U-hit.V,hit.U,hit.V);
            auto surface=material.SampleSurface ? material.SampleSurface(triangle.UV[0]*bary.x+triangle.UV[1]*bary.y+triangle.UV[2]*bary.z) : material.Surface;
            if (!Finite(surface.Albedo) || !Finite(surface.Emission))
            { error="Material evaluation returned non-finite GI data."; return false; }
            surface.Albedo*=triangle.Color[0]*bary.x+triangle.Color[1]*bary.y+triangle.Color[2]*bary.z;
            surface.Albedo=glm::clamp(surface.Albedo,glm::vec3(0),glm::vec3(1));
            const auto interpolated=triangle.N[0]*bary.x+triangle.N[1]*bary.y+triangle.N[2]*bary.z;
            auto n=glm::length(interpolated)>1e-8f ? glm::normalize(interpolated) : triangle.Face;
            auto face=triangle.Face;
            if (glm::dot(face,path.Direction)>0) face=-face;
            if (glm::dot(n,face)<0) n=-n;
            const auto point=triangle.P[0]*bary.x+triangle.P[1]*bary.y+triangle.P[2]*bary.z;
            path.Radiance+=path.Throughput*glm::max(surface.Emission,glm::vec3(0));
            auto addLight=[&](glm::vec3 direction, float distance, glm::vec3 radiance) {
                const float cosine=std::max(0.0f,glm::dot(n,direction));
                if (cosine==0 || !glm::any(glm::greaterThan(radiance,glm::vec3(0)))) return true;
                visibilityRays.push_back({point+face*options.Bias,distance,direction});
                visibilityPaths.push_back(active[i]);
                contributions.push_back(path.Throughput*surface.Albedo*radiance*(cosine/3.14159265359f));
                return visibilityRays.size()<65536 || flushVisibility();
            };
            if (!addLight(light,std::numeric_limits<float>::max(),options.LightRadiance)) return false;
            for (const auto& lamp : options.BounceLights)
            {
                glm::vec3 toLight=-glm::normalize(lamp.Direction);
                float attenuation=1, distance=std::numeric_limits<float>::max();
                if (lamp.Kind!=LightmapBounceLight::Type::Directional)
                {
                    const auto delta=lamp.Position-point;
                    distance=glm::length(delta);
                    if (distance<=options.Bias || distance>=lamp.Range) continue;
                    toLight=delta/distance;
                    const float fraction=distance/lamp.Range;
                    const float smooth=std::max(0.0f,1-fraction*fraction);
                    attenuation=smooth*smooth/std::max(1.0f,distance*distance);
                    if (lamp.Kind==LightmapBounceLight::Type::Spot)
                        attenuation*=std::clamp((glm::dot(-toLight,glm::normalize(lamp.Direction))-std::cos(lamp.OuterAngle))/
                            (std::cos(lamp.InnerAngle)-std::cos(lamp.OuterAngle)),0.0f,1.0f);
                }
                if (!addLight(toLight,distance-options.Bias,lamp.Radiance*attenuation)) return false;
            }
            path.Throughput*=surface.Albedo;
            path.Active=!glm::all(glm::lessThan(path.Throughput,glm::vec3(1e-5f)));
            if (!path.Active || bounce+1==options.IndirectBounces) { path.Active=false; continue; }
            path.Origin=point+face*options.Bias;
            auto random=[&]() { path.Seed=path.Seed*1664525u+1013904223u; return float(path.Seed>>8u)*(1.0f/16777216.0f); };
            const float radial=random(), azimuth=random();
            path.Direction=Hemisphere(n,radial,azimuth);
            path.Active=path.Active && glm::dot(path.Direction,face)>0;
        }
        if (!flushVisibility()) return false;
    }
    for (size_t i=0; i<samples.size(); ++i)
    {
        glm::vec3 irradiance(0);
        for (uint32_t p=0; p<options.IndirectSamples; ++p)
        {
            const auto radiance=paths[i*options.IndirectSamples+p].Radiance;
            irradiance+=radiance;
            if (!moments.empty()) { const double l=Luminance(radiance); moments[samples[i].Pixel]+=l*l; }
        }
        irradiance/=float(options.IndirectSamples);
        if (!Finite(irradiance)) { error="GI radiance overflow; reduce light/emission intensities."; return false; }
        for (int c=0; c<3; ++c) result.IndirectRGB[samples[i].Pixel*3+c]+=irradiance[c];
    }
    return true;
}
float Cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }
}

float BlendShadowLightmap(float realtime, float baked, float fade, ShadowLightmapMode mode)
{
    fade = std::clamp(fade, 0.0f, 1.0f);
    switch (mode)
    {
    case ShadowLightmapMode::Replace: return realtime * (1 - fade) + baked * fade;
    case ShadowLightmapMode::Overlay: return baked * (realtime * (1 - fade) + fade);
    case ShadowLightmapMode::Only: return baked;
    default: return realtime;
    }
}

bool BakeShadowLightmap(const ShadowLightmapMesh& receiver, std::span<const ShadowLightmapMesh> occluders,
                        const ShadowLightmapBakeOptions& options, ShadowLightmapBakeResult& output, std::string& error)
{
    error.clear();
    auto fail = [&](const char* message) { error = message; return false; };
    if (!options.Width || !options.Height || options.Width > kMaxLightmapResolution || options.Height > kMaxLightmapResolution ||
        !options.RaysPerSample || options.RaysPerSample > 4096 || options.Padding > 64 ||
        !Finite(options.DirectionToLight) || !std::isfinite(glm::length(options.DirectionToLight)) ||
        glm::length(options.DirectionToLight) < 1e-8f || !std::isfinite(options.Bias) || options.Bias <= 0 ||
        !std::isfinite(options.AngularRadiusRadians) || options.AngularRadiusRadians < 0 || options.AngularRadiusRadians >= 1.5f)
        return fail("Invalid shadow lightmap size, ray count, padding, bias, or light direction/radius.");
    if (options.IndirectSamples > 4096 || options.IndirectBounces > 16 ||
        (options.IndirectSamples && !options.IndirectBounces) ||
        !Finite(options.LightRadiance) || !Finite(options.EnvironmentRadiance) ||
        glm::any(glm::lessThan(options.LightRadiance, glm::vec3(0))) ||
        glm::any(glm::lessThan(options.EnvironmentRadiance, glm::vec3(0))))
        return fail("Invalid GI sample count, bounce count, or radiance.");
    for (const auto& light : options.BounceLights)
        if (!Finite(light.Position) || !Finite(light.Direction) || glm::length(light.Direction) < 1e-8f ||
            !Finite(light.Radiance) || glm::any(glm::lessThan(light.Radiance,glm::vec3(0))) ||
            !std::isfinite(light.Range) || light.Range <= 0 || !std::isfinite(light.InnerAngle) ||
            !std::isfinite(light.OuterAngle) || light.InnerAngle < 0 || light.OuterAngle <= light.InnerAngle || light.OuterAngle >= 3.14159f)
            return fail("Invalid bounce light parameters.");
    std::vector<Triangle> receivers, casters;
    std::vector<size_t> materials;
    if (!Gather(receiver, true, receivers, error)) return false;
    for (size_t i = 0; i < occluders.size(); ++i)
    {
        const auto& caster = occluders[i];
        if (!Finite(caster.Surface.Albedo) || !Finite(caster.Surface.Emission))
            return fail("Non-finite bake material.");
        if (!Gather(caster, false, casters, error)) return false;
        materials.resize(casters.size(), i);
    }
    if (receivers.empty()) return fail("Receiver has no non-degenerate triangles.");
    std::vector<float> positions;
    for (const auto& tri : casters)
        for (auto p : tri.P) { positions.push_back(p.x); positions.push_back(p.y); positions.push_back(p.z); }
    SceneBvh::MeshGeometryView view{};
    view.Positions = positions;
    std::vector<uint32_t> triangleIds(casters.size());
    for (size_t i = 0; i < casters.size(); ++i) triangleIds[i] = uint32_t(i);
    view.TriangleMaterials = triangleIds;
    const auto bvh = SceneBvh::ThreadedBvhBuilder::Build(view);
    if (!casters.empty() && bvh.IsEmpty()) return fail("Could not build shadow caster BVH.");
    ShadowLightmapBakeResult result;
    result.Width = options.Width; result.Height = options.Height;
    const size_t pixels = size_t(options.Width) * options.Height;
    result.Visibility.assign(pixels, 255);
    std::vector<float> sums(pixels, 0);
    if (options.IndirectSamples) result.IndirectRGB.assign(pixels * 3, 0);
    std::vector<uint8_t> coverage(pixels, 0);
    const bool denoise=options.IndirectSamples && options.Denoise;
    const auto charts=denoise ? FindCharts(receivers) : std::vector<uint32_t>{};
    std::vector<DenoiseGuide> guides(denoise ? pixels : 0);
    std::vector<double> moments(denoise ? pixels : 0,0);
    // One owner per subtexel: adjacent triangle edges use a consistent first owner.
    std::vector<int32_t> owners(pixels * 4, -1);
    LightmapRayBatch accelerated;
    if (options.Continue && !options.Continue()) return fail("Shadow lightmap bake cancelled.");
    if (options.CreateRayTracer && !positions.empty()) accelerated=options.CreateRayTracer(positions);
    if (options.AccelerationChanged) options.AccelerationChanged(bool(accelerated));
    LightmapRayBatch trace=[&](std::span<const LightmapRay> rays, std::span<LightmapRayHit> hits) {
        constexpr size_t batchSize=65536;
        for (size_t offset=0; offset<rays.size(); offset+=batchSize)
        {
            if (options.Continue && !options.Continue()) return false;
            const auto batch=rays.subspan(offset,std::min(batchSize,rays.size()-offset));
            const auto out=hits.subspan(offset,batch.size());
            if (accelerated && accelerated(batch,out)) result.GpuTracedRays+=batch.size();
            else
            {
                if (accelerated)
                {
                    accelerated={};
                    if (options.AccelerationChanged) options.AccelerationChanged(false);
                }
                for (size_t i=0; i<batch.size(); ++i)
                {
                    if ((i&255u)==0 && options.Continue && !options.Continue()) return false;
                    Hit hit; hit.Distance=batch[i].MaxDistance;
                    out[i]={};
                    if (Trace(bvh,batch[i].Origin,batch[i].Direction,&hit,batch[i].MinDistance))
                        out[i]={hit.Distance,hit.Bary.y,hit.Bary.z,hit.Triangle};
                }
            }
            result.TracedRays+=batch.size();
        }
        return true;
    };
    std::vector<BakeSample> samples;
    // Bound temporary path memory independently of atlas resolution/sample count.
    const size_t sampleBatch=std::min<size_t>(256,65536/std::max({1u,options.IndirectSamples,
        options.AngularRadiusRadians==0 ? 1u : options.RaysPerSample}));
    auto flushSamples=[&]() {
        if (samples.empty()) return true;
        const bool ok=IntegrateSamples(samples,casters,materials,occluders,options,trace,result,sums,moments,error);
        samples.clear();
        return ok;
    };
    if (options.Progress) options.Progress(0);
    for (size_t ti = 0; ti < receivers.size(); ++ti)
    {
        if (options.Continue && !options.Continue()) return fail("Shadow lightmap bake cancelled.");
        const auto& tri = receivers[ti];
        const auto a = tri.UV[1] - tri.UV[0], b = tri.UV[2] - tri.UV[0];
        const float area = Cross(a, b);
        if (std::abs(area) < 1e-12f) return fail("Receiver contains a degenerate UV1 triangle.");
        const auto dpdu=((tri.P[1]-tri.P[0])*b.y-(tri.P[2]-tri.P[0])*a.y)/area;
        const auto dpdv=((tri.P[2]-tri.P[0])*a.x-(tri.P[1]-tri.P[0])*b.x)/area;
        const float footprint=std::max(glm::length(dpdu)/options.Width,glm::length(dpdv)/options.Height);
        const auto uvmin = glm::min(tri.UV[0], glm::min(tri.UV[1], tri.UV[2]));
        const auto uvmax = glm::max(tri.UV[0], glm::max(tri.UV[1], tri.UV[2]));
        const int minx = std::max(0, int(std::floor(uvmin.x * options.Width)));
        const int miny = std::max(0, int(std::floor(uvmin.y * options.Height)));
        const int maxx = std::min(int(options.Width) - 1, int(std::floor(uvmax.x * options.Width)));
        const int maxy = std::min(int(options.Height) - 1, int(std::floor(uvmax.y * options.Height)));
        const auto geometricNormal = tri.Face;
        for (int y = miny; y <= maxy; ++y)
        {
        if (options.Continue && !options.Continue()) return fail("Shadow lightmap bake cancelled.");
        if (options.Progress) options.Progress(0.9f*(ti+float(y-miny)/std::max(1,maxy-miny+1))/receivers.size());
        for (int x = minx; x <= maxx; ++x)
        for (int sample = 0; sample < 4; ++sample)
        {
            const glm::vec2 uv((x + 0.25f + (sample & 1) * 0.5f) / options.Width,
                               (y + 0.25f + (sample >> 1) * 0.5f) / options.Height);
            const float v = Cross(uv - tri.UV[0], b) / area;
            const float w = Cross(a, uv - tri.UV[0]) / area;
            const float u = 1 - v - w;
            if (std::min({u,v,w}) < -1e-6f) continue;
            const size_t pixel = size_t(y) * options.Width + x;
            auto& owner = owners[pixel * 4 + sample];
            if (owner >= 0)
            {
                // A shared edge is legal; overlapping chart interiors are not.
                if (std::min({u,v,w}) > 1e-5f) return fail("Overlapping UV1 charts detected; unwrap the receiver before baking.");
                continue;
            }
            owner = int32_t(ti);
            uint32_t seed=uint32_t(pixel*4+sample)*747796405u+2891336453u;
            seed=((seed>>((seed>>28u)+4u))^seed)*277803737u;
            const auto position=tri.P[0]*u+tri.P[1]*v+tri.P[2]*w;
            const auto interpolatedNormal=tri.N[0]*u+tri.N[1]*v+tri.N[2]*w;
            const auto shadingNormal=glm::length(interpolatedNormal)>1e-8f ? glm::normalize(interpolatedNormal) : geometricNormal;
            if (denoise)
            {
                auto& guide=guides[pixel];
                guide.Position+=position; guide.Normal+=shadingNormal;
                guide.Footprint=std::max(guide.Footprint,footprint);
                if (!coverage[pixel]) guide.Chart=charts[ti];
                else if (guide.Chart!=charts[ti]) guide.Chart=UINT32_MAX;
            }
            ++coverage[pixel];
            samples.push_back({pixel,seed,position,geometricNormal,shadingNormal});
            if (samples.size()>=sampleBatch && !flushSamples()) return false;
        }
        }
    }
    if (!flushSamples()) return false;
    for (size_t i = 0; i < pixels; ++i)
        if (coverage[i])
        {
            result.Visibility[i] = uint8_t(std::lround(255 * sums[i] / coverage[i]));
            if (options.IndirectSamples)
                for (int c=0; c<3; ++c) result.IndirectRGB[i*3+c] /= coverage[i];
            ++result.CoveredTexels;
        }
    if (!result.CoveredTexels) return fail("No receiver texels covered; increase bake resolution.");
    if (denoise)
    {
        std::vector<float> variance(pixels,0);
        for (size_t i=0; i<pixels; ++i)
        {
            if (!coverage[i]) continue;
            auto& guide=guides[i];
            guide.Position/=coverage[i];
            guide.Normal=glm::length(guide.Normal)>1e-8f ? glm::normalize(guide.Normal) : glm::vec3(0,1,0);
            const double count=double(coverage[i])*options.IndirectSamples;
            const double mean=Luminance(glm::vec3(result.IndirectRGB[i*3],result.IndirectRGB[i*3+1],result.IndirectRGB[i*3+2]));
            variance[i]=float(std::clamp((moments[i]/count-mean*mean)/std::max(1.0,count-1),0.0,1e30));
        }
        if (!DenoiseDiffuse(result,coverage,guides,std::move(variance),options.Continue,options.Progress))
            return fail("Shadow lightmap bake cancelled.");
    }
    // Dilate only into empty texels. Existing chart interiors never get blurred.
    for (uint32_t pass = 0; pass < options.Padding; ++pass)
    {
        if (options.Continue && !options.Continue()) return fail("Shadow lightmap bake cancelled.");
        if (options.Progress) options.Progress(0.98f+0.02f*pass/options.Padding);
        auto nextCoverage = coverage;
        auto nextVisibility = result.Visibility;
        auto nextIndirect = result.IndirectRGB;
        for (uint32_t y = 0; y < options.Height; ++y)
        for (uint32_t x = 0; x < options.Width; ++x)
        {
            const size_t i = size_t(y) * options.Width + x;
            if (coverage[i]) continue;
            unsigned sum = 0, count = 0;
            glm::vec3 indirect(0);
            auto add = [&](size_t j) {
                if (coverage[j])
                {
                    sum += result.Visibility[j]; ++count;
                    if (options.IndirectSamples)
                        for (int c=0; c<3; ++c) indirect[c] += result.IndirectRGB[j*3+c];
                }
            };
            if (x) add(i-1); if (x+1 < options.Width) add(i+1);
            if (y) add(i-options.Width); if (y+1 < options.Height) add(i+options.Width);
            if (count)
            {
                nextCoverage[i] = 1; nextVisibility[i] = uint8_t((sum + count/2) / count);
                if (options.IndirectSamples)
                    for (int c=0; c<3; ++c) nextIndirect[i*3+c] = indirect[c]/count;
            }
        }
        coverage.swap(nextCoverage); result.Visibility.swap(nextVisibility); result.IndirectRGB.swap(nextIndirect);
    }
    if (options.Continue && !options.Continue()) return fail("Shadow lightmap bake cancelled.");
    output = std::move(result);
    if (options.Progress) options.Progress(1);
    return true;
}
}
