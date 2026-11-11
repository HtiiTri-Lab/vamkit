# shader-lift

Readers for Unity 2018.1's serialized data in the user's own VaM install: asset bundles (UnityFS), `.assets` files and the
`Shader` objects inside them, down to each sub-program's D3D11 bytecode (DXBC), keywords, constant buffer layouts and
texture / buffer slots. The generators of `skin`, `rt-core`, `cloth-shading` and `hair-strands` build on it to find VaM's
own pixel programs and to know what the path tracer must handle. Python standard library only; read-only on VaM; nothing it
produces is kept in the repository. The manifest summary also says "lifts DXBC to HLSL": that step is not here, it is
`components/shading/skin/tools/asm_lift.py`.

## Files

| File | Holds |
|---|---|
| `bundle_audit.py` | Minimal UnityFS reader (LZMA and LZ4 blocks, type trees): lists render-relevant components (lights, reflection probes, cameras, renderers, materials, shaders) with key fields from a `.var` or asset bundle; the primitive layer the others import as `B` |
| `find_shader_tt.py` | Finds the `Shader` (class 48) type tree in VaM's `StreamingAssets` bundles and saves it as JSON |
| `parse_shader.py` | Parses a Shader object from an `.assets` file that has no type trees, using that tree: passes, stages, sub-programs, keywords, constant buffer members, bindings (`load_type_tree()` makes the tree on first use) |
| `shader_extract.py` | Extracts the DXBC programs of shaders matched by a name regex, plus an index with each program's keywords |

## Build

None (`build = "none"`). Run the scripts with `python -I`. Environment: `VAMKIT_VAM` (the VaM install folder, the one holding
`VaM_Data`) for `find_shader_tt.py` and for everything that calls `load_type_tree()`.

## Settings

None. Output location: `find_shader_tt.py` writes `<repo>\out\gen\shader-lift\shader_tt.json` (or the path you give it); the
generators' own outputs go under `<repo>\out\gen\<component>\`.

## Interfaces

- Command lines: `python -I bundle_audit.py <file.var|file.assetbundle> [inner-path-substring]`;
  `python -I find_shader_tt.py [<StreamingAssets dir> [<out.json>]]` (defaults `%VAMKIT_VAM%\VaM_Data\StreamingAssets` and the path above);
  `python -I parse_shader.py <assets> <pathid> <shader_tt.json> <out.json>`;
  `python -I shader_extract.py <assets file> <shader-name regex> <out dir>`.
- Imported by `components/shading/skin/tools/skin_shaders.py` (puts this folder on `sys.path`), and through it by
  `components/shading/rt-core/tools/*`, `components/shading/cloth-shading/tools/gen_cloth_programs.py`.
- Programs are identified by the SHA-1 of their bytecode; consumers key their replacement tables on its first 8 bytes.

## Tests

None offline: they are used and exercised by the generators and their tests (`skin/tests`, `cloth-shading/tests`). A new VaM build
shows up as a generator failing to find a shader name, a hash that no table knows, or the type tree not found.

## Invariants

- Read only. Never write into the VaM folder and never commit what these scripts extract (DXBC, type trees, JSON summaries).
- Unity 2018.1's serialization only; another Unity version needs the readers checked first (`bundle_audit.py` has the class-id table).
- Keep the readers free of third-party Python packages.

## Read next

[../../../docs/knowledge/skin.md](../../../docs/knowledge/skin.md), [../../../docs/knowledge/vam-facts.md](../../../docs/knowledge/vam-facts.md)
