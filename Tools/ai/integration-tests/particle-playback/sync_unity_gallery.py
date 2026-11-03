#!/usr/bin/env python3
"""Refresh supported Unity noise/gradient/collision settings in an existing native gallery.
Requires PyYAML and the original licensed source pack. Existing files are backed up.
This preserves the scene's geometry, materials, layout, and unrelated authoring.
"""
import argparse
import copy
import json
import math
from pathlib import Path
import re
import shutil


def read_objects(path):
    import yaml

    text = re.sub(r'^%.*\n', '', path.read_text(), flags=re.M)
    objects = {}
    for header, body in re.findall(r'^--- !u!\d+ &([^\n]+)\n(.*?)(?=^---|\Z)', text, re.M | re.S):
        if ' stripped' in header:
            continue
        obj = yaml.safe_load(body)
        if obj:
            objects[int(header)] = next(iter(obj.values()))
    return objects


def curve(value):
    mode = value.get('minMaxState', 0)
    a, b = value.get('minScalar', 0), value.get('scalar', 0)
    if mode == 0:
        return {'mode': 'constant', 'min': b}
    if mode == 3:
        return {'mode': 'range', 'min': min(a, b), 'max': max(a, b)}
    raise ValueError('Noise uses a curve distribution not supported by this gallery sync')


def sync_render_settings(block, source_name, source, document):
    """Map renderer features that cannot be expressed as simulation processors."""
    fields = {}
    if source_name == 'FX_env_Wind':
        trail = source['TrailModule']
        width = curve(trail['widthOverTrail'])
        fields = {'trailOnly': True, 'trailInheritColor': bool(trail['inheritParticleColor']),
                  'trailSizeAffectsWidth': bool(trail['sizeAffectsWidth']),
                  'trailDieWithParticle': bool(trail['dieWithParticles']),
                  'trailMinVertexDistance': trail['minVertexDistance'],
                  'trailSegments': 256, 'trailWidthMin': width['min'],
                  'trailWidthMax': width.get('max', width['min']),
                  'trailFadeOverParticleLifetime': True, 'trailFadeOverLength': True,
                  'trailAlphaPeak': trail['colorOverTrail']['maxGradient']['key1']['a'],
                  'trailTextureTile': trail['textureMode'] == 1,
                  'trailTextureScaleU': trail['textureScale']['x'],
                  'trailTextureScaleV': trail['textureScale']['y']}
        for phase in document['phases']:
            phase['processors'] = [p for p in phase['processors'] if p.get('label') != 'Trail width approximation']
            for proc in phase['processors']:
                if proc.get('label') == 'Initial velocity':
                    # Single-sided edges emit along local +Y; source rotation X=90 maps this to +Z.
                    proc['values'] = [{'mode': 'constant', 'min': v} for v in (0, 0, source['InitialModule']['startSpeed']['scalar'])]
    for field, value in fields.items():
        line = 'ParticleEmitter3D.' + field + ' = ' + json.dumps(value)
        pattern = r'^ParticleEmitter3D\.' + field + r' = .*'
        if re.search(pattern, block, re.M):
            block = re.sub(pattern, line, block, flags=re.M)
        else:
            block = block.replace('ParticleStack.enabled', line + '\nParticleStack.enabled')
    return block


def triangulate_obj_face(line):
    """Convert the gallery's convex OBJ faces to native triangle input offline."""
    face, marker, comment = line.partition('#')
    tokens = face.split()
    if not tokens or tokens[0] != 'f':
        return [line]
    corners = tokens[1:]
    if len(corners) < 3:
        raise ValueError('Gallery OBJ face needs at least three corners')
    if len(corners) == 3:
        return [line]
    suffix = ' #' + comment if marker else ''
    return ['f ' + ' '.join((corners[0], corners[i], corners[i + 1])) + suffix
            for i in range(1, len(corners) - 1)]


def write_converted_mesh(path, backup, converted):
    """Accept the original or our previous output; never overwrite later edits."""
    current = path.read_text()
    if current == converted:
        return
    if current != backup.read_text():
        raise ValueError(f'Refusing to overwrite edited mesh: {path}. '
                         f'Preserved the current file and original backup {backup}.')
    path.write_text(converted)


def sync_render_assets(scenes, source_assets):
    """Keep licensed geometry/textures external; update only known converted assets."""
    assets = scenes.parent
    material = assets / 'materials/fxm_butterfly_00.material'
    if material.exists():
        backup = material.with_suffix('.material.before-render-sync')
        if not backup.exists():
            shutil.copyfile(material, backup)
        doc = json.loads(material.read_text())
        unity = next(o for o in read_objects(source_assets / 'Materials/FXm_Butterfly_00.mat').values() if 'm_SavedProperties' in o)
        floats = {k: v for item in unity['m_SavedProperties']['m_Floats'] for k, v in item.items()}
        colors = {k: v for item in unity['m_SavedProperties']['m_Colors'] for k, v in item.items()}
        props = doc['properties']
        props.update(wingFlutter=1, wingSpeed=floats['_WingSpeed'], wingMin=floats['_WingRangeMin'],
                     wingMax=floats['_WingRangeMax'], gpuFogSurfaceDepthFade=0, gpuFogCameraDepthFadeRange=0)
        for native, source in [('wingColor1', '_WingsColor1'), ('wingColor2', '_WingsColor2')]:
            props[native] = [colors[source][c] for c in 'rgba']
        mask = assets / 'textures/fxt_gradient_01.png'
        if not mask.exists():
            shutil.copyfile(source_assets / 'Textures/vol1_textures/FXt_Gradient_01.png', mask)
        doc['textures']['wingMaskMap'] = {'path': '../textures/fxt_gradient_01.png'}
        material.write_text(json.dumps(doc, indent=2) + '\n')
    butterfly = assets / 'models/vfx_butterfly_00.obj'
    if butterfly.exists():
        backup = butterfly.with_suffix('.obj.before-uv-sync')
        if not backup.exists():
            shutil.copyfile(butterfly, backup)
        lines = []
        for line in backup.read_text().splitlines():
            if line.startswith('vt '):
                values = line.split()
                line = f'vt {values[1]} {1 - float(values[2]):.9g}'
            lines.extend(triangulate_obj_face(line))
        converted = '\n'.join(lines) + '\n'
        write_converted_mesh(butterfly, backup, converted)
    mesh = assets / 'models/quad_10_0_(0_0,-0_5,0_0)_IndependentAxes.obj'
    if mesh.exists():
        backup = mesh.with_suffix('.obj.before-render-sync')
        if not backup.exists():
            shutil.copyfile(mesh, backup)
        # Unity BakeMesh: pivot Y=-0.5 gives Y=-1..0 with UV.y=0 at the top.
        # Native image UV.y starts at the top, so invert V once when writing the mesh.
        write_converted_mesh(mesh, backup, 'v -0.5 -1.0 0.0\nv 0.5 -1.0 0.0\nv 0.5 0.0 0.0\nv -0.5 0.0 0.0\n'
                        'vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nf 1/1 2/2 3/3\nf 1/1 3/3 4/4\n')

def sync_scene(path, sources, report):
    text = path.read_text()
    blocks = re.split(r'(?=\[entity id=)', text)
    roots = {}
    for block in blocks:
        root = re.match(r'\[entity id="(gallery\d+)_effect"', block)
        name = re.search(r'Name.value = "([^"]+)"', block)
        if root and name:
            roots[root[1]] = name[1]
    fallback = path.stem.removeprefix('Cozy_')
    if not fallback.startswith('FX_'):
        fallback = 'FX_env_' + fallback
    changed = 0
    for index, block in enumerate(blocks):
        entity = re.match(r'\[entity id="(?:(gallery\d+)_)?u(\d+)"', block)
        match = re.search(r'ParticleStack.definition = "(.*)"', block)
        if not entity or not match:
            continue
        source_name = roots.get(entity[1], fallback)
        objects = sources.get(source_name, {})
        game_id = int(entity[2])
        source = next((o for o in objects.values() if o.get('m_GameObject', {}).get('fileID') == game_id and 'InitialModule' in o), None)
        if source is None:
            report['unmatched'].append({'scene': path.name, 'entity': entity[0], 'prefab': source_name})
            continue
        document = json.loads(match[1])
        before = copy.deepcopy(document)
        noise = source.get('NoiseModule', {})
        for phase in document['phases']:
            for proc in phase['processors']:
                if proc.get('kind') == 'collision':
                    collision = source.get('CollisionModule', {})
                    if collision.get('enabled'):
                        proc['collisionDamping'] = collision.get('m_Dampen', {}).get('scalar', 0)
                if proc.get('kind') == 'noise' and noise.get('enabled'):
                    strength = noise['strength']['scalar']
                    axes = [strength, noise['strengthY']['scalar'], noise['strengthZ']['scalar']] if noise['separateAxes'] else [strength] * 3
                    position = curve(noise['positionAmount'])
                    # The native curl helper takes a spatial wavelength, not frequency.
                    frequency = max(noise['frequency'], .001)
                    proc.update(label='Turbulent motion', noiseMode='velocity', noiseAxes=axes,
                                noiseAlgorithm='perlin', noiseQuality={0: 'low', 1: 'medium', 2: 'high'}[noise['quality']],
                                noiseDamping=bool(noise['damping']), noiseOctaveMultiplier=noise['octaveMultiplier'],
                                noiseOctaveScale=noise['octaveScale'],
                                noiseOctaves=noise['octaves'], noiseSize=noise['sizeAmount']['scalar'],
                                noiseRotation=math.radians(max(abs(noise['rotationAmount']['scalar']), abs(noise['rotationAmount'].get('minScalar', 0)))),
                                values=[position, {'mode': 'constant', 'min': 1 / frequency}, curve(noise['scrollSpeed'])])
                    report['noise'].append({'scene': path.name, 'prefab': source_name, 'axes': axes,
                                            'frequency': frequency, 'octaves': noise['octaves'], 'quality': noise['quality'],
                                            'damping': bool(noise['damping']), 'octaveMultiplier': noise['octaveMultiplier'],
                                            'octaveScale': noise['octaveScale']})
                if proc.get('stage') == 'birth' and proc.get('target') == 'color' and source['InitialModule']['startColor']['minMaxState'] == 4:
                    proc['driver'] = 'random'
                # Unity type=1 is collision; do not spawn a landed particle when a leaf expires in air.
                if proc.get('kind') == 'event' and proc.get('action') == 'emit':
                    subs = source.get('SubModule', {}).get('subEmitters', [])
                    for sub in subs:
                        child = objects.get(sub['emitter']['fileID'], {})
                        child_object = objects.get(child.get('m_GameObject', {}).get('fileID'), {})
                        if proc.get('emitterPath', '').split('/')[-1] == child_object.get('m_Name'):
                            proc['trigger'] = {0: 'birth', 1: 'collision', 2: 'death'}.get(sub['type'], proc['trigger'])
                            distance_rate = child.get('EmissionModule', {}).get('rateOverDistance', {}).get('scalar', 0)
                            if sub['type'] == 0 and distance_rate > 0:
                                proc.update(trigger='distance', threshold=1 / distance_rate, count=1, killSource=False)

        rendered_block = sync_render_settings(block, source_name, source, document)
        render_changed = rendered_block != block
        block = rendered_block
        match = re.search(r'ParticleStack.definition = \"(.*)\"', block)
        if document != before or render_changed:
            blocks[index] = block[:match.start(1)] + json.dumps(document, separators=(',', ':')) + block[match.end(1):]
            changed += 1
    if changed:
        backup = path.with_suffix(path.suffix + '.before-unity-sync')
        if not backup.exists():
            shutil.copyfile(path, backup)
        path.write_text(''.join(blocks))
    report['scenes'][path.name] = changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-assets', type=Path, required=True)
    parser.add_argument('--scenes', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    paths = list(args.source_assets.rglob('*.prefab'))
    by_guid = {}
    for path in paths:
        meta = path.with_suffix('.prefab.meta')
        if meta.exists():
            match = re.search(r'^guid: (\w+)', meta.read_text(), re.M)
            if match:
                by_guid[match[1]] = path
    cache = {}
    def resolve(path):
        if path in cache:
            return copy.deepcopy(cache[path])
        objects = read_objects(path)
        merged = {}
        for obj in objects.values():
            if 'm_SourcePrefab' not in obj:
                continue
            base = by_guid.get(obj['m_SourcePrefab'].get('guid'))
            if base is None:
                raise ValueError('Missing source prefab: ' + str(path))
            inherited = resolve(base)
            for mod in obj['m_Modification']['m_Modifications']:
                target = inherited.get(mod['target']['fileID'])
                if target is None:
                    continue
                keys = mod['propertyPath'].replace('.Array.data[', '.').replace(']', '').split('.')
                try:
                    for key in keys[:-1]:
                        target = target[int(key)] if isinstance(target, list) else target[key]
                    key = int(keys[-1]) if isinstance(target, list) else keys[-1]
                    old = target[key]
                    value = mod.get('value', '')
                    target[key] = int(value) if isinstance(old, int) else float(value) if isinstance(old, float) else value
                except (KeyError, IndexError, ValueError, TypeError):
                    # Object references and unrelated serialized fields are not changed by this sync.
                    continue
            merged.update(inherited)
        merged.update({i: o for i, o in objects.items() if 'm_SourcePrefab' not in o})
        cache[path] = merged
        return copy.deepcopy(merged)
    sources = {p.stem: resolve(p) for p in paths}
    report = {'scenes': {}, 'noise': [], 'unmatched': [], 'limitations': [
        'Perlin curl, quality, damping and octave controls are mapped. Unity internal seeds/gradient tables are not replicated; trajectories are not bit-identical.'
    ]}
    sync_render_assets(args.scenes, args.source_assets)
    for scene in sorted(args.scenes.glob('*.scene')):
        sync_scene(scene, sources, report)
    report['limitations'] = sorted(set(report['limitations']))
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'scenes': report['scenes'], 'unmatched': len(report['unmatched']), 'limitations': report['limitations']}, indent=2))


if __name__ == '__main__':
    main()
