#!/usr/bin/env python3
"""Generate a native playback preview using a locally licensed Cozy VFX pack.

No source assets are modified or added to the repository. The output is generated
content; rerunning replaces its preview scene and materials.
"""
import argparse
import json
from pathlib import Path
import shutil
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-assets', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    assets = args.output.resolve() / 'Assets'
    for folder in ('Textures', 'Materials', 'Models', 'Scenes'):
        (assets / folder).mkdir(parents=True, exist_ok=True)

    def reference(relative):
        guid = uuid.uuid5(uuid.NAMESPACE_URL, 'particle-playback/' + relative)
        return f'[path="{relative}" guid="{guid}"]'

    def material(name, texture=None, **properties):
        document = dict(schemaVersion=3, materialName=name, lightingModel='Unlit',
                        alphaMode='Blend', doubleSided=True, zWrite=False,
                        surfaceShader='Surfaces/particle_surface.glsl',
                        properties=dict(baseColor=[1, 1, 1, 1], gpuFogSurfaceDepthFade=0,
                                        gpuFogCameraDepthFadeRange=0, **properties),
                        textures={}, blend=dict(srcColor='One', dstColor='OneMinusSrcAlpha',
                                                srcAlpha='One', dstAlpha='OneMinusSrcAlpha'))
        if texture:
            source = next(args.source_assets.rglob(texture), None)
            if source is None:
                raise SystemExit(f'Missing required texture: {texture}')
            shutil.copyfile(source, assets / 'Textures' / texture)
            document['textures']['albedoMap'] = {'path': '../Textures/' + texture}
        path = 'Materials/' + name + '.material'
        (assets / path).write_text(json.dumps(document, indent=2) + '\n')
        return reference(path)

    def constant(value):
        return dict(mode='constant', min=value)

    def curve(points):
        return dict(mode='curve', minKeys=[dict(time=t, value=v, interpolation='linear') for t, v in points])

    def processor(label, **fields):
        values = fields.pop('values', None)
        result = dict(label=label, kind='property', stage='birth')
        result.update(fields)
        if values is not None:
            result['values'] = [v if isinstance(v, dict) else constant(v) for v in values]
        return result

    def stack(processors):
        return dict(version=1, entryPhase=1, phases=[dict(id=1, label='Playback', processors=[
            dict(id=i + 2, **p) for i, p in enumerate(processors)])])

    chunks = ['[scene name="ParticlePlayback" version=1]\n']

    def entity(identifier, name, parent=None, position=(0, 0, 0), **fields):
        heading = f'[entity id="{identifier}"' + (f' parent="{parent}"' if parent else '') + ']'
        lines = [heading, 'Name.value = ' + json.dumps(name),
                 'Transform.position = (' + ', '.join(map(str, position)) + ')',
                 'Transform.rotation = (0, 0, 0, 1)']
        fields.setdefault('Transform.scale', (1, 1, 1))
        for key, value in fields.items():
            if isinstance(value, bool):
                value = str(value).lower()
            elif isinstance(value, (tuple, list)):
                value = '(' + ', '.join(map(str, value)) + ')'
            elif isinstance(value, dict):
                # Scene strings wrap the JSON verbatim, without JSON string escaping.
                value = '"' + json.dumps(value, separators=(',', ':')) + '"'
            lines.append(f'{key} = {value}')
        chunks.append('\n'.join(lines) + '\n')

    entity('sky', 'Sky', **{'SkyEnvironment.Mode': 1, 'SkyEnvironment.AutoSunMoon': False,
           'SkyEnvironment.GradientSkyTopColor': (.04, .05, .07),
           'SkyEnvironment.GradientSkyHorizonColor': (.08, .09, .12),
           'SkyEnvironment.GradientSkyBottomColor': (.03, .03, .04),
           'SkyEnvironment.GradientSkyIntensity': 200})
    entity('camera', 'Camera', position=(0, 3, -12), **{'Camera.perspective': True,
           'Camera.fovY': 60, 'Camera.nearZ': .01, 'Camera.farZ': 200,
           'Camera.exposureControl': 1, 'Camera.manualExposureEV': 7})
    ground = dict(schemaVersion=3, materialName='Ground', lightingModel='Unlit', alphaMode='Opaque',
                  surfaceShader='Surfaces/unlit_solid.glsl', properties=dict(baseColor=[.035, .045, .055, 1]))
    (assets / 'Materials/Ground.material').write_text(json.dumps(ground, indent=2) + '\n')
    entity('ground', 'Ground', position=(0, -.03, 0), **{'Transform.scale': (30, 1, 30),
           'MeshRenderer.meshPrimitive': 'Plane', 'MeshRenderer.material': reference('Materials/Ground.material')})

    # XY card, two metres high after size=2; pivot raises its lower edge to y=0.
    card = assets / 'Models/Flame.obj'
    card.write_text('v -0.5 0 0\nv 0.5 0 0\nv 0.5 1.25 0\nv -0.5 1.25 0\n'
                    'vt 0 1\nvt 1 1\nvt 1 0\nvt 0 0\nf 1/1 2/2 3/3\nf 1/1 3/3 4/4\n')
    for index, (texture, tint, size, columns) in enumerate([
        ('FXt_Fire_00.png', [1, .05, .01, 1], 2, 4),
        ('FXt_Fire_01.png', [1, .35, .03, 1], 1.6, 5),
        ('FXt_Fire_02.png', [1, .8, .1, 1], 1.2, 4),
    ]):
        mat = material('Flame' + str(index), texture, alphaFromGrayscale=1, hybridAlphaBlend=1)
        processors = [processor('Continuous emission', kind='emitRate', stage='emission', values=[1]),
                      processor('Lifetime', target='lifetime', operation='set', values=[1.25]),
                      processor('Size', target='size', operation='set', values=[size]),
                      processor('Color', target='color', operation='set', values=tint),
                      processor('Stationary flame', target='velocity', operation='set', values=[0, 0, 0]),
                      processor('Start transparent', target='alpha', operation='set', values=[0]),
                      processor('Overlap crossfade', stage='update', target='alpha', operation='set',
                                values=[curve([(0, 0), (.2, 1), (.8, 1), (1, 0)])])]
        entity('flame' + str(index), 'Continuous Flame ' + str(index + 1), position=(-3, 0, 0), **{
            'ParticleEmitter3D.enabled': True, 'ParticleEmitter3D.emitting': True,
            'ParticleEmitter3D.amount': 16, 'ParticleEmitter3D.lifetime': 1.25,
            'ParticleEmitter3D.preProcessTime': 5, 'ParticleEmitter3D.fixedFps': 60,
            'ParticleEmitter3D.useFixedSeed': True, 'ParticleEmitter3D.seed': 10 + index,
            'ParticleEmitter3D.transformAlign': 1, 'ParticleEmitter3D.drawPass0.mesh': reference('Models/Flame.obj'),
            'ParticleEmitter3D.material': mat, 'ParticleEmitter3D.texture': reference('Textures/' + texture),
            'ParticleEmitter3D.flipbookColumns': columns, 'ParticleEmitter3D.flipbookRows': 4,
            'ParticleEmitter3D.flipbookBlend': False, 'ParticleEmitter3D.flipbookFramesPerSecond': 0,
            'ParticleStack.enabled': True, 'ParticleStack.definition': stack(processors)})

    leaf = material('Leaves', 'FXt_Leaves.png', alphaFromGrayscale=1, hybridAlphaBlend=1)
    entity('leaves', 'Leaf landing', position=(3, 0, 0))
    common = {'ParticleEmitter3D.enabled': True, 'ParticleEmitter3D.amount': 128,
              'ParticleEmitter3D.fixedFps': 60, 'ParticleEmitter3D.useFixedSeed': True,
              'ParticleEmitter3D.seed': 42, 'ParticleEmitter3D.material': leaf,
              'ParticleEmitter3D.texture': reference('Textures/FXt_Leaves.png'),
              'ParticleEmitter3D.flipbookColumns': 2, 'ParticleEmitter3D.flipbookRows': 2,
              'ParticleEmitter3D.flipbookStartFrame': 3, 'ParticleEmitter3D.flipbookBlend': False,
              'ParticleStack.enabled': True}
    spawn = [processor('Emit leaves', kind='emitRate', stage='emission', values=[5]),
             processor('Lifetime', target='lifetime', operation='set', values=[10]),
             processor('Leaf size', target='size', operation='set', values=[dict(mode='range', min=.3, max=.5)]),
             processor('Gradient color', target='color', operation='set', driver='random',
                       values=[curve([(0, .7924528), (1, .5294118)]), curve([(0, .58394843), (1, .24169973)]),
                               curve([(0, .115877524), (1, .15686272)]), 1]),
             processor('Tilted emission rectangle', kind='shape', shape='meshSurface', offset=[0, 5, 0],
                       vertices=[[-2.5, -1.767767, -1.767767], [2.5, -1.767767, -1.767767],
                                 [2.5, 1.767767, 1.767767], [-2.5, 1.767767, 1.767767]], indices=[0, 1, 2, 0, 2, 3]),
             processor('Initial direction', target='velocity', operation='set', values=[0, -.707107, .707107]),
             processor('Initial rotation', target='rotation', operation='set', values=[dict(mode='range', min=0, max=6.283185)]),
             processor('Downward force', kind='acceleration', stage='update', values=[0, dict(mode='range', min=-.75, max=-.5), 0]),
             processor('Horizontal flutter', kind='noise', stage='update', noiseMode='velocity', noiseAxes=[1, 0, 1],
                       noiseAlgorithm='perlin', noiseQuality='high', noiseDamping=True,
                       noiseOctaves=2, noiseRotation=.785398, values=[dict(mode='range', min=-1, max=1), 4, 1]),
             processor('Ground collision', kind='collision', stage='update', threshold=.25, probability=0,
                       killSource=True, values=[0, 1, 0, 0]),
             processor('Fixed frame', target='animationSpeed', operation='set', values=[0]),
             processor('Land', kind='event', stage='event', trigger='collision', action='emit', emitterPath='../Landed',
                       inherit=2 | 4 | 8, count=1)]
    entity('falling', 'Falling', parent='leaves', **common, **{
        'ParticleEmitter3D.transformAlign': 5, 'ParticleEmitter3D.emitting': True,
        'ParticleEmitter3D.lifetime': 10, 'ParticleEmitter3D.preProcessTime': 10,
        'ParticleStack.definition': stack(spawn)})
    landed = [processor('Lifetime', target='lifetime', operation='set', values=[5]),
              processor('Authored child size', target='size', operation='set', values=[1.75]),
              processor('Authored child color', target='color', operation='set', values=[1, 1, 1, 1]),
              processor('Rest', target='velocity', operation='set', values=[0, 0, 0]),
              processor('Fixed frame', target='animationSpeed', operation='set', values=[0]),
              processor('Fade out', stage='update', target='alpha', operation='multiply', basis='birth',
                        values=[curve([(0, 1), (.81471, 1), (1, 0)])])]
    entity('landed', 'Landed', parent='leaves', **common, **{
        'ParticleEmitter3D.transformAlign': 6, 'ParticleEmitter3D.emitting': False,
        'ParticleEmitter3D.lifetime': 5, 'ParticleStack.definition': stack(landed)})
    scene = assets / 'Scenes/ParticlePlayback.scene'
    scene.write_text('\n'.join(chunks))
    print(scene)


if __name__ == '__main__':
    main()
