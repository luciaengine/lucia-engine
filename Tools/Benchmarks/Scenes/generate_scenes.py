#!/usr/bin/env python3
"""Writes the performance benchmark project: six scenes, their materials, the UI of the 2D scene and
benchmark.json, the manifest with each scene's camera poses and run settings.

Usage: python3 Tools/Benchmarks/Scenes/generate_scenes.py [<project dir>]
The default project dir is Tests/Projects/PerformanceBenchmark. The output is deterministic: run it
after a change here and commit the result. Content that is too large for the repository (the city's
cluster meshes, the crowd's model) is made or copied by benchmark.py when it prepares a run.
"""

import json
import math
import random
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_PROJECT = REPO_ROOT / "Tests" / "Projects" / "PerformanceBenchmark"

# Each scene's run: frames per pose. The frame counts are the same for every scene so that the
# noise of two scenes compares.
RUN = {"warmupFrames": 300, "timingFrames": 600, "profiledFrames": 120}
RESOLUTION = [1920, 1080]

CITY_UNIQUE_BUILDINGS = 32
CITY_GRID = 16


def fmt(value):
    text = f"{value:.4f}".rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


def vec(values):
    return "(" + ", ".join(fmt(v) for v in values) + ")"


def yaw_quat(deg):
    half = math.radians(deg) * 0.5
    return (0.0, math.sin(half), 0.0, math.cos(half))


def axis_quat(axis, deg):
    half = math.radians(deg) * 0.5
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


class Scene:
    def __init__(self, name, comment):
        self.name = name
        self.lines = [f'[scene name="{name}" version=1]', f"; {comment}", ""]

    def entity(self, eid, name, fields, position=(0, 0, 0), rotation=(0, 0, 0, 1), scale=(1, 1, 1)):
        self.lines.append(f'[entity id="{eid}"]')
        self.lines.extend(fields)
        self.lines.append(f'Name.value = "{name}"')
        self.lines.append(f"Transform.position = {vec(position)}")
        self.lines.append(f"Transform.rotation = {vec(rotation)}")
        self.lines.append(f"Transform.scale = {vec(scale)}")
        self.lines.append("")

    def camera(self, far, ev, position=(0, 2, -5), perspective=True, ortho_size=10.0):
        fields = ["Camera.enabled = true", f"Camera.perspective = {'true' if perspective else 'false'}",
                  "Camera.fovY = 60", "Camera.nearZ = 0.1", f"Camera.farZ = {fmt(far)}",
                  "Camera.exposureControl = 1", f"Camera.manualExposureEV = {fmt(ev)}"]
        if not perspective:
            fields.append(f"Camera.orthographicSize = {fmt(ortho_size)}")
        self.entity("e_cam", "Main Camera", fields, position)

    def sun(self, pitch_deg, yaw_deg, intensity=3.0, shadows=True):
        rotation = quat_mul(yaw_quat(yaw_deg), axis_quat((1, 0, 0), pitch_deg))
        self.entity("e_sun", "Sun", ["Light.enabled = true", "Light.type = 0", "Light.color = (1, 0.96, 0.9)",
                                     f"Light.intensity = {fmt(intensity)}", "Light.castsLight = true",
                                     f"Light.castsShadows = {'true' if shadows else 'false'}",
                                     "Light.cascadeCount = 4"], rotation=rotation)

    def sky(self):
        self.entity("e_sky", "Sky", ["SkyEnvironment.AutoSunMoon = false"])

    def mesh(self, eid, primitive, material, position, scale=(1, 1, 1), rotation=(0, 0, 0, 1), shadows=True):
        self.entity(eid, eid, [f"MeshRenderer.meshPrimitive = {primitive}",
                               f'MeshRenderer.material = [path="Materials/{material}.material"]',
                               "MeshRenderer.renderLayers = [0]",
                               f"MeshRenderer.shadowCasting = {'On' if shadows else 'Off'}",
                               "MeshRenderer.receiveShadows = true"], position, rotation, scale)

    def write(self, project):
        path = project / "Assets" / "Scenes" / f"{self.name}.scene"
        path.write_text("\n".join(self.lines).rstrip() + "\n", encoding="utf-8")


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def pose(name, position, yaw, pitch):
    return {"name": name, "position": list(position), "yawDeg": yaw, "pitchDeg": pitch}


def ground(scene, size, material="Ground"):
    scene.mesh("e_ground", "Plane", material, (0, 0, 0), (size, 1, size), shadows=False)


def forest():
    s = Scene("Forest", "Dense forest: wind-animated procedural trees, a sun with virtual shadow maps.")
    s.camera(far=600, ev=13)
    s.sky()
    s.sun(pitch_deg=50, yaw_deg=30)
    s.entity("e_wind", "Wind", ["WindVolume.isGlobal = true", "WindVolume.shape = 0", "WindVolume.weight = 1",
                                "WindVolume.direction = (1, 0, 0.35)", "WindVolume.speed = 4",
                                "WindVolume.turbulence = 0.35", "WindVolume.gustFrequency = 0.5",
                                "WindVolume.gustScale = 70"])
    ground(s, 400)
    rng = random.Random(1001)
    spacing, half = 9.0, 13
    for gz in range(-half, half + 1):
        for gx in range(-half, half + 1):
            if abs(gx) <= 1 and abs(gz) <= 1:
                continue  # a clearing for the first pose
            x = gx * spacing + rng.uniform(-3.0, 3.0)
            z = gz * spacing + rng.uniform(-3.0, 3.0)
            seed = 1 + (gx * 31 + gz * 17) % 12
            s.entity(f"e_tree_{gx + half}_{gz + half}", "Tree", [f"EZTree.seed = {seed}"], (x, 0, z),
                     yaw_quat(rng.uniform(0, 360)), (1, 1, 1))
    return s, {
        "description": "Dense forest: 720 procedural trees from 12 seeds, global wind, a sun with virtual shadows.",
        "pipeline": {"virtualShadows": True},
        "requiredPasses": ["Shadow"],
        "poses": [pose("clearing", (0, 1.7, 0), 20, 2), pose("canopy", (0, 45, -130), 0, -18),
                  pose("edge", (-125, 2, -125), 45, 4)],
    }


def open_world():
    s = Scene("OpenWorld", "Open world: generated terrain, an ocean, scattered rocks under an HLOD volume, long view.")
    s.camera(far=12000, ev=14)
    s.sky()
    s.sun(pitch_deg=35, yaw_deg=-40, intensity=4)
    s.entity("e_terrain", "Terrain", ["Terrain.terrainAssetType = 0", "Terrain.baseSource = 2", "Terrain.enabled = true",
                                      "Terrain.sizeX = 4096", "Terrain.sizeZ = 4096", "Terrain.heightScale = 400",
                                      "Terrain.samplesPerMeter = 1", "Terrain.domain = 0",
                                      "Terrain.targetPixelError = 4", "Terrain.renderLayers = [0]",
                                      "Terrain.castShadows = true", "Terrain.receiveShadows = true"], (-2048, -60, -2048))
    s.entity("e_terrain_noise", "Terrain Noise", ["TerrainModifierVolume.shape = 4", "TerrainNoiseEffect.enabled = true",
                                                  "TerrainNoiseEffect.mode = 1", "TerrainNoiseEffect.preset = 0"])
    s.entity("e_ocean", "Ocean", ["OceanSurface.Enabled = true", "OceanSurface.Roughness = 0.12",
                                  "OceanRenderer.Enabled = true", "OceanRenderer.GlobalWindSpeed = 11",
                                  "OceanRenderer.GlobalWindDirection = 25"])
    s.entity("e_hlod", "HLOD Volume", ["HLODVolume.CellSize = 64", "HLODVolume.MinMembers = 8"])
    rng = random.Random(2002)
    for i in range(1500):
        x, z = rng.uniform(-1800, 1800), rng.uniform(-1800, 1800)
        size = rng.uniform(2.0, 9.0)
        primitive = "Sphere" if i % 3 else "Cube"
        s.mesh(f"e_rock_{i}", primitive, "Rock", (x, 20 + rng.uniform(0, 40), z), (size, size * 0.6, size),
               yaw_quat(rng.uniform(0, 360)))
    return s, {
        "description": "Open world: a 4 km generated terrain, an ocean, 1500 scattered rocks under an HLOD volume.",
        "requiredPasses": ["Ocean"],
        "notes": "HLOD proxies come from a bake (the editor's Bake HLOD or a packaged cook). A loose-asset run draws the rocks without proxies.",
        "poses": [pose("shore", (0, 15, -1500), 0, -4), pose("vista", (-1600, 400, -1600), 45, -10),
                  pose("low", (500, 40, 0), 270, -2)],
    }


def crowd():
    s = Scene("Crowd", "Crowd: 400 animated skinned characters with bone LOD.")
    s.camera(far=500, ev=13)
    s.sky()
    s.sun(pitch_deg=45, yaw_deg=60)
    ground(s, 300)
    clips = ["Run", "Walk", "Survey"]
    rng = random.Random(3003)
    for i in range(400):
        row, col = divmod(i, 20)
        x = (col - 9.5) * 3.0 + rng.uniform(-0.6, 0.6)
        z = row * 3.0 + rng.uniform(-0.6, 0.6)
        s.entity(f"e_fox_{i}", "Fox", ['MeshRenderer.meshAsset = [path="Models/Fox/Fox.glb"]',
                                       "MeshRenderer.renderLayers = [0]", "MeshRenderer.shadowCasting = On",
                                       "MeshRenderer.receiveShadows = true", "Animator.active = true",
                                       "Animator.loop = true", 'Animator.source = "Clip"',
                                       f'Animator.assignedAnimation = "{clips[i % 3]}"',
                                       "Animator.autoPlayOnEnterPlayMode = true", "BoneLOD.switchCoverage = 0.05"],
                 (x, 0, z), yaw_quat(rng.uniform(0, 360)), (0.025, 0.025, 0.025))
    return s, {
        "description": "Crowd: 400 skinned foxes playing three clips, each with bone LOD.",
        "content": ["fox"],
        "requiredPasses": [],
        "poses": [pose("close", (0, 1.6, -6), 0, -5), pose("mid", (0, 12, -25), 0, -20),
                  pose("overview", (-60, 40, -40), 45, -25)],
    }


def city():
    s = Scene("City", "City: unique procedural buildings drawn through cluster LOD.")
    s.camera(far=3000, ev=14)
    s.sky()
    s.sun(pitch_deg=40, yaw_deg=20, intensity=4)
    ground(s, 1200, "Asphalt")
    rng = random.Random(4004)
    block = 36.0
    for gz in range(CITY_GRID):
        for gx in range(CITY_GRID):
            index = (gx * 7 + gz * 13) % CITY_UNIQUE_BUILDINGS
            x = (gx - (CITY_GRID - 1) / 2) * block
            z = (gz - (CITY_GRID - 1) / 2) * block
            s.entity(f"e_building_{gx}_{gz}", "Building",
                     [f'MeshRenderer.meshAsset = [path="Models/City/Building{index:02d}.gecluster"]',
                      'MeshRenderer.material = [path="Materials/Concrete.material"]', "MeshRenderer.renderLayers = [0]",
                      "MeshRenderer.shadowCasting = On", "MeshRenderer.receiveShadows = true"],
                     (x, 0, z), yaw_quat(90 * rng.randrange(4)))
    return s, {
        "description": f"City: {CITY_GRID * CITY_GRID} buildings from {CITY_UNIQUE_BUILDINGS} unique procedural meshes, cooked to cluster LOD.",
        "content": ["city"],
        "environment": {"GE_CLUSTER_LOD": "1"},
        "requiredPasses": [],
        "poses": [pose("street", (18, 1.7, -270), 0, 2), pose("rooftop", (-300, 120, -300), 45, -15),
                  pose("aerial", (0, 600, -700), 0, -38)],
    }


def interior():
    s = Scene("Interior", "Interior close-up: local lights, DDGI and reflection probes in a closed hall.")
    s.camera(far=150, ev=7)
    s.entity("e_sky", "Sky", ["SkyEnvironment.IblIntensity = 0.02", "SkyEnvironment.GradientSkyIntensity = 40",
                              "SkyEnvironment.AutoSunMoon = false"])
    walls = [("floor", (0, -0.1, 0), (24.4, 0.2, 24.4), "White"), ("ceiling", (0, 6.1, 0), (24.4, 0.2, 24.4), "White"),
             ("left", (-12.1, 3, 0), (0.2, 6.4, 24.4), "Red"), ("right", (12.1, 3, 0), (0.2, 6.4, 24.4), "Blue"),
             ("back", (0, 3, 12.1), (24.4, 6.4, 0.2), "White"), ("front", (0, 3, -12.1), (24.4, 6.4, 0.2), "White")]
    for name, position, scale, material in walls:
        s.mesh(f"e_wall_{name}", "Cube", material, position, scale)
    for i in range(36):
        x, z = -10 + (i % 6) * 4, -10 + (i // 6) * 4
        if i % 2:
            s.mesh(f"e_pillar_{i}", "Cube", "White", (x, 2.5, z), (0.5, 5, 0.5))
        else:
            s.mesh(f"e_sphere_{i}", "Sphere", "Metal" if i % 4 else "Red", (x, 0.6, z), (1.2, 1.2, 1.2))
    for i in range(48):
        x, z = -11 + (i % 8) * 3.14, -11 + (i // 8) * 4.4
        s.entity(f"e_light_{i}", f"Light {i}", ["Light.enabled = true", "Light.type = 1",
                                                "Light.color = (1, 0.88, 0.7)", "Light.intensity = 25",
                                                "Light.range = 7", "Light.castsLight = true",
                                                f"Light.castsShadows = {'true' if i % 12 == 0 else 'false'}"],
                 (x, 5.4, z))
    s.entity("e_ddgi", "DDGI Volume", ["DDGIVolume.Enabled = true", "DDGIVolume.ProbePlacement = 0",
                                       "DDGIVolume.Divisions = 12", "DDGIVolume.RaysPerProbe = 128"],
             (0, 3, 0), scale=(23.6, 5.8, 23.6))
    for i, x in enumerate((-6, 6)):
        s.entity(f"e_probe_{i}", f"Reflection Probe {i}", ["ReflectionProbe.Enabled = true",
                                                           "ReflectionProbe.InfluenceShape = Box",
                                                           "ReflectionProbe.BlendDistance = 2",
                                                           "ReflectionProbe.CaptureResolution = 256",
                                                           "ReflectionProbe.UpdateMode = Realtime",
                                                           "ReflectionProbe.RealtimeUpdateInterval = 0.5"],
                 (x, 3, 0), scale=(12.2, 6.2, 24.2))
    return s, {
        "description": "Interior: a closed 24 m hall, 48 point lights (4 with shadows), a DDGI volume and 2 realtime reflection probes.",
        "requiredPasses": ["DDGI"],
        "poses": [pose("closeup", (-1.5, 1.2, -5), 20, -8), pose("hall", (0, 3, -11.5), 0, -6),
                  pose("corner", (10.5, 5, 10.5), 225, -25)],
    }


def ui_2d():
    s = Scene("Sprites2D", "2D and UI: alpha-blended unlit quads under an orthographic camera and a dense editor-style UI.")
    s.camera(far=100, ev=0, position=(0, 0, -20), perspective=False, ortho_size=12)
    rng = random.Random(6006)
    face_camera = axis_quat((1, 0, 0), -90)
    for i in range(5000):
        x, y = rng.uniform(-22, 22), rng.uniform(-12, 12)
        size = rng.uniform(0.2, 1.4)
        depth = rng.uniform(0, 10)
        s.mesh(f"e_sprite_{i}", "Plane", f"Sprite{i % 6}", (x, y, depth), (size, 1, size), face_camera, shadows=False)
    s.entity("e_ui", "Editor UI", ["UIDocument.Enabled = true", 'UIDocument.Layout = [path="UI/BenchmarkPanels.uxml"]',
                                   'UIDocument.Style = [path="UI/BenchmarkPanels.css"]', "UIDocument.SortOrder = 0",
                                   "UIDocument.RenderMode = Overlay"])
    return s, {
        "description": "2D and UI: 5000 alpha-blended unlit quads, orthographic, under 3 UI panels of 60 rows each.",
        "requiredPasses": ["UI"],
        "poses": [pose("full", (0, 0, -20), 0, 0), pose("zoomed", (8, 4, -20), 0, 0)],
    }


def materials():
    def pbr(name, color, metallic=0.0, roughness=0.8):
        return {"schemaVersion": 3, "materialName": name, "lightingModel": "StandardPBR", "alphaMode": "Opaque",
                "doubleSided": False, "surfaceShader": "Surfaces/standard_pbr.glsl",
                "properties": {"baseColor": color, "metallic": metallic, "roughness": roughness}, "textures": {}}

    result = {"Ground": pbr("Ground", [0.22, 0.27, 0.16, 1.0], roughness=0.95),
              "Rock": pbr("Rock", [0.42, 0.40, 0.37, 1.0], roughness=0.9),
              "Asphalt": pbr("Asphalt", [0.12, 0.12, 0.13, 1.0], roughness=0.9),
              "Concrete": pbr("Concrete", [0.6, 0.58, 0.55, 1.0], roughness=0.85),
              "White": pbr("White", [0.75, 0.75, 0.72, 1.0], roughness=0.9),
              "Red": pbr("Red", [0.63, 0.06, 0.05, 1.0], roughness=0.9),
              "Blue": pbr("Blue", [0.06, 0.12, 0.6, 1.0], roughness=0.9),
              "Metal": pbr("Metal", [0.95, 0.93, 0.88, 1.0], metallic=1.0, roughness=0.15)}
    colors = [[1.0, 0.4, 0.3], [0.3, 0.8, 1.0], [0.5, 1.0, 0.4], [1.0, 0.85, 0.3], [0.8, 0.4, 1.0], [1.0, 1.0, 1.0]]
    for i, color in enumerate(colors):
        result[f"Sprite{i}"] = {"schemaVersion": 3, "materialName": f"Sprite{i}", "lightingModel": "Unlit",
                                "alphaMode": "Blend", "doubleSided": True, "surfaceShader": "Surfaces/unlit_solid.glsl",
                                "properties": {"baseColor": color + [1.0], "opacity": 0.7}, "textures": {}}
    return result


def ui_files():
    rows = []
    for panel in range(3):
        rows.append(f'    <uielement class="panel panel-{panel}">')
        rows.append(f'        <label class="panel-title" text="Panel {panel + 1}" />')
        for row in range(60):
            value = (panel * 37 + row * 11) % 100
            rows.append('        <uielement class="row">')
            rows.append(f'            <label class="row-name" text="Property {row + 1:02d}" />')
            rows.append(f'            <progress class="row-bar" value="{value}" max="100" />')
            rows.append(f'            <label class="row-value" text="{value}.{row % 10}" />')
            rows.append("        </uielement>")
        rows.append("    </uielement>")
    uxml = '<uielement class="root">\n' + "\n".join(rows) + "\n</uielement>\n"
    css = """.root {
    position: absolute;
    left: 0;
    top: 0;
    width: 100%;
    height: 100%;
    display: flex;
    flex-direction: row;
    gap: 8px;
    padding: 8px;
}

.panel {
    display: flex;
    flex-direction: column;
    flex-grow: 1;
    gap: 1px;
    padding: 6px;
    background-color: rgba(28, 30, 36, 0.86);
    border: 1px solid rgba(90, 96, 110, 0.9);
    border-radius: 4px;
}

.panel-title {
    color: rgba(220, 224, 232, 1);
    font-size: 14px;
}

.row {
    display: flex;
    flex-direction: row;
    align-items: center;
    gap: 6px;
    height: 15px;
}

.row-name {
    color: rgba(190, 194, 202, 1);
    font-size: 12px;
    width: 110px;
}

.row-bar {
    flex-grow: 1;
    height: 8px;
}

.row-value {
    color: rgba(150, 200, 255, 1);
    font-size: 12px;
    width: 48px;
}
"""
    return uxml, css


def main():
    project = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else DEFAULT_PROJECT
    for folder in ("Scenes", "Materials", "UI"):
        (project / "Assets" / folder).mkdir(parents=True, exist_ok=True)

    scenes = {}
    for builder in (forest, open_world, crowd, city, interior, ui_2d):
        scene, settings = builder()
        scene.write(project)
        scenes[scene.name] = {"scene": f"Scenes/{scene.name}.scene", **settings}

    for name, material in materials().items():
        (project / "Assets" / "Materials" / f"{name}.material").write_text(json.dumps(material, indent=2) + "\n",
                                                                           encoding="utf-8")
    uxml, css = ui_files()
    (project / "Assets" / "UI" / "BenchmarkPanels.uxml").write_text(uxml, encoding="utf-8")
    (project / "Assets" / "UI" / "BenchmarkPanels.css").write_text(css, encoding="utf-8")

    manifest = {"resolution": RESOLUTION, "run": RUN,
                "city": {"uniqueBuildings": CITY_UNIQUE_BUILDINGS}, "scenes": scenes}
    (project / "benchmark.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(scenes)} scenes to {project}")


if __name__ == "__main__":
    main()
