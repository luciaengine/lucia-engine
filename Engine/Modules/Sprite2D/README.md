# Sprite2D

Sprite frame assets, atlas rectangles and pivots, and time-based frame selection live in this module. Public headers are `Sprite2D/SpriteFrames.h` and `Sprite2D/SpritePlayback.h`, under `GameEngine::Sprite2D`.

The Engine consumer advances playback and extracts ECS artwork. Sprite2D has no skeletal animation or animation graph dependency. It links AssetCore, Types, Serialization, JSON and Logger. Tests cover validated asset loading, optional pivots, frame boundaries, reverse playback and looping.
