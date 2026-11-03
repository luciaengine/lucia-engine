# Derived runtime scene subsets

This headless tool uses SceneIO's canonical parser and immutable prepared scene.
It never creates ECS entities, starts graphics, resolves assets, edits the authored
scene or supplies project-specific grouping rules.

Build `SceneSubsetCook`. Inspect a scene into a **new** output directory:

```sh
SceneSubsetCook --scene /project/Assets/Scenes/Main.scene --output /build/catalog
```

`catalog.json` lists authored IDs, names, component names, parents and conservative
dependency IDs in document order. Pass a JSON array of exact IDs to derive a
closed subset and its catalog:

```sh
SceneSubsetCook --scene /project/Assets/Scenes/Main.scene \
  --selection /build/selected-ids.json --output /build/subset
```

For a whole project, use `--groups /build/groups.json` instead of `--selection`.
The JSON object maps lowercase portable group names to arrays of exact entity IDs.
The engine builds its full reference catalog once, closes each group independently,
then emits `<group>.scene` files into a new directory. Every emitted document is
reparsed and canonically reserialized byte-for-byte before `complete.json` is
written. Its group records include IDs and byte counts; downstream build receipts
must still bind cryptographic hashes and source/tool revisions. Group overlap is
allowed by this generic tool and must be resolved by the project's ownership plan.
Invalid roots/names reject the batch before creating output. A later write failure
leaves a partial generation without the completion marker.

The tool includes parents and transitive references, including quoted strings
matching entity IDs. This may conservatively include an ordinary string's matching
entity. Explicit `$id` and nested array/tuple references are included too. File,
resource and asset references are not mistaken for entity IDs. The full resource
table and exact component property strings are retained; no schema save function
can replace unknown fields or change their numeric precision. Captured includes
are expanded, and editor-only hierarchy/camera snapshots are omitted.

Limits and integration obligations:

- Flat scenes only. Blueprints, subscenes, embeds and unparseable values fail
  before output, with no synchronous expansion fallback.
- Script/name-based gameplay relationships are not inferred. The project must
  explicitly supply all semantic roots, including shared and interior dependencies.
- Independently selected groups can overlap. The caller must assign shared
  ownership or provide cross-group binding before activating multiple groups.
- `scene.scene` keeps relative paths unchanged. Preserve the original asset root
  and scene-directory resolution semantics when packaging/loading it.
- `complete.json` means this tool finished writing, not a certified build receipt.
  Build integration must verify hashes, game/engine versions and source freshness.
- Existing outputs and symlinked output ancestry are rejected. Keep the output
  parent exclusively owned; this is not an adversarial filesystem sandbox.
- The output is derived build data, not a replacement authored scene. Loading it
  does not by itself implement async presentation, resource readiness or transitions.

Run `test_cli.py --tool /absolute/path/to/SceneSubsetCook` for nine headless
CLI tests. SceneIO's `PreparedProjection_*` tests cover closure, references,
captured inputs, raw values, worker lifetime and staged publication.
