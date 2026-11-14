# Planar reflections and rain

Open this project in the editor, open `Assets/Scenes/PlanarPuddles.scene`, and choose `RenderPipelines/RainyCourtyard.rendergraph` in Project Settings → Rendering. Switch to Game View to use the authored camera.

The pipeline uses the current ForwardPlus graph, a planar capture/composite after opaque rendering, and procedural rain after temporal AA. RainBackground copies the scene into the rain target every frame before additive rain draws; loading an uninitialized or previous-frame target accumulates invalid streaks and loses the scene.

The camera uses fixed exposure 0.002 for the scene's physical lighting. Camera lens rain stays off. The ground material controls the puddle mask and ripple normals; PlanarReflection controls capture resolution and roughness acceptance.
