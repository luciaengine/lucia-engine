import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { proxyTool } from "./proxy.js";

export const tools: ToolDef[] = [
  proxyTool({
    name: "bake_lightmaps",
    category: "scene",
    description:
      "Requires the optional lightmap-baking package to be enabled. Bake diffuse GI and directional shadow lightmaps in the background for explicitly selected static meshes. Receivers require non-overlapping UV1; only opaque Standard PBR is supported. Receivers also cast into the bake. Select static occluders explicitly, and pass the primary directional light. The environment is explicit: provide linear environmentRadiance or an HDR panorama via environmentTexture; omitted means black. GPU ray queries are selected automatically when available, with CPU fallback. Direct lights and reflections remain live. Returns a job ID; poll get_lightmap_bake for progress and results. The editor imports completed outputs and applies requested materials automatically. Shares one job with the Panels > Lighting Bake UI. Stop playback before baking, and save the scene after completion. No automatic UV unwrapping or procedural-sky capture. See docs/Rendering/lightmap-baking.html.",
    schema: {
      receivers: z.array(z.coerce.number().int().min(0).max(4294967295)).min(1).max(4096),
      occluders: z.array(z.coerce.number().int().min(0).max(4294967295)).max(4096),
      lightEntity: z.coerce.number().int().min(0).max(4294967295),
      resolution: z.coerce.number().int().min(1).max(8192).optional().describe("Default 1024, maximum 8192 (8K); 64 is only a fast workflow preview"),
      giSamples: z.coerce.number().int().min(0).max(4096).optional().describe("Default 256; zero bakes shadows only"),
      useGpu: z.boolean().optional().describe("Default true: use GPU ray queries when available, with automatic CPU fallback; false forces CPU tracing"),
      denoise: z.boolean().optional().describe("Default true: filter GI using variance, UV charts, positions and normals; preserves directional shadow edges"),
      bounces: z.coerce.number().int().min(1).max(16).optional(),
      shadowSamples: z.coerce.number().int().min(1).max(4096).optional(),
      padding: z.coerce.number().int().min(0).max(64).optional(),
      bias: z.coerce.number().positive().optional(),
      environmentRadiance: z.array(z.coerce.number().nonnegative()).length(3).optional(),
      environmentTexture: z.string().optional().describe("Source panorama image path or GUID; overrides uniform environmentRadiance"),
      environmentIntensity: z.coerce.number().nonnegative().optional(),
      environmentRotationDegrees: z.coerce.number().optional(),
      shadowMode: z.enum(["Replace", "Overlay", "Only"]).optional(),
      fadeStart: z.coerce.number().nonnegative().optional().describe("View depth where live-shadow fade begins; default 40"),
      fadeEnd: z.coerce.number().positive().optional().describe("Fade end must exceed fadeStart and stay within live shadow distance; default 50"),
      apply: z.boolean().optional().describe("Default true: assign generated materials on completion if scene inputs still match"),
    },
  }),
  proxyTool({
    name: "get_lightmap_bake",
    category: "scene",
    description:
      "Read the shared editor lightmap job: progress in [0,1], completed/total receiver counts, acceleration backend and cancellation state while baking; output paths, actual GPU/total ray counts and assignment status on completion. The editor imports and applies completed bakes even without polling or an open Lighting Bake panel. Repeated completed polls return the same result until another job or scene change. Save the scene to persist the new material references.",
    schema: {},
  }),
  proxyTool({
    name: "cancel_lightmap_bake",
    category: "scene",
    description: "Request cancellation of the active lightmap bake. Poll get_lightmap_bake to observe cancellation. Completed output from prior jobs is preserved.",
    schema: {},
  }),
];
