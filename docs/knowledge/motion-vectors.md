# Motion vectors and depth: where every kind of element gets them

DLSS SR, RR, the neural denoiser and DLSS Frame Generation all need, for every pixel, a depth and a motion vector (where
that surface point was in the previous frame). VaM draws its characters in a way Unity cannot see: skin and clothing are skinned
on the GPU into world-space compute buffers and drawn with `Graphics.DrawMesh`; hair is expanded by tessellation from a buffer
of guide strands. Unity therefore produces no motion for them, and a wrong vector shows up directly: contours that ripple like
heat haze, hair that shimmers, a face that deforms in generated frames. What a player sees is a character who moves smoothly
in the reconstructed picture; what the engineer maintains is a rule for which surface owns each pixel's depth and motion.

The rule, in one line: **a pixel's depth and motion come from the opaque surface it really shows; a transparent thing
carries neither, but a person's own see-through clothing does; the character's own motion wins over everything except UI.**

## How it works

### Baseline

For every pixel the guide shader (`guideShader` in `components/render/opencomposite/OpenOVR/Compositor/dx11_vam_dlss.cpp`)
takes the scene depth, rebuilds the surface point from it and moves it by `clipToPrevClip` (sent by the bridge each frame, previous
projection x previous view x inverse view x inverse projection, with the jitter removed): that is the camera's motion. Everything below
replaces that value where something else owns the pixel. Unity's own motion vectors (`DepthTextureMode.MotionVectors`) are not
enabled; the bridge never asks for them.

The result per eye: R32F depth, RG32F motion without jitter (SR, RR), motion with the jitter added (NR), RG16F motion
(frame generation, shared with OFXR), a UI mask.

### Raster route (SR K / L / M): the compositor redraws

The bridge (`components/render/dlss-bridge/VaMDLSSBridge.cs`) collects what drew this frame and the compositor redraws each of them
twice, with this and the previous frame's positions, into a motion target with its own depth buffer and stencil
(`RenderObjectMotion`, `objectShader`), keeping only fragments whose depth matches the scene depth.
Draws are collected by Harmony hooks and per-atom lists, never by a timed scene search (a 2-second `FindObjectsOfType` once caused
periodic hitches in a big scene).

| Element | Where the bridge finds it | What the compositor draws | Depth match |
|---|---|---|---|
| Skin (`DAZSkinV2`) | postfix on its GPU draw; vertex and index buffer | the mesh at current and previous vertex positions (`kDraw` flags 0) | 1.5 cm or 3% |
| Clothing, simulated clothing, mesh hair (`DAZSkinWrap`) | postfix on its GPU draw | same | 1.5 cm or 3% |
| Physics hair strands (GPUTools `HairRender`) | `HairRender.Awake` postfix, plus one search when hair motion starts; the tessellated guide point buffer, `TessSegments` points per strand, valid count = guides x `TessSegments` (stride 56 bytes) | one camera-facing ribbon per guide segment, 2 cm wide, segments longer than 12 cm dropped, drawn with this and last frame's points (`kDrawHair`) | 2.5 cm or 2% |
| Props, toys, CUA meshes (`MeshRenderer`) | the atoms' own renderer lists (`Atom._meshRenderers`, `_dynamicMeshRenderers`), re-read on `Atom.Init`, (de)register, add and remove; each frame every `localToWorld`; only visible ones that moved are sent | the mesh's own vertex buffer with the matrix pair (`kDrawRigid`) | 1.5 cm or 3% |
| Animated CUAs (`SkinnedMeshRenderer`: a fan in a hand, dancers) | `CustomUnityAssetLoader.DoLoadedCallbacks` postfix collects them; each frame every bone's skinning matrix (`localToWorld` x bind pose), this and last frame; bind-pose positions and bone weights once per mesh (readable meshes only; blend shapes are not followed) | skinned twice in the vertex shader (`kDrawSkinned`) | 1.5 cm or 3% |
| World-space UI, menus | not collected on this route (no UI quads) | menus are rendered as native 100% frames with SR skipped while VaM's own menus are open (`DLSSMenuNative`) | |
| Transparent and additive things: beams, haze, particles, glass, glitter | not collected | nothing: the pixel keeps the motion of the surface behind | |
| Anything else (terrain, walls, static props) | | camera motion from depth | |

Order inside `RenderObjectMotion`: skin, clothing and mesh hair, then moving props and animated CUAs (all with "nearest wins" depth testing
and a stencil mark, so the nearest of them owns the pixel by depth), then hair ribbons only where the stencil is still 0, then, when enabled,
UI. The stencil is a depth-ordering aid, not a priority of people over props: see Tried and dropped.

Only **opaque** props are sent: a submesh counts if one of its materials has `renderQueue <= 2500` (Geometry and AlphaTest write
depth); `ShadowsOnly` renderers are skipped; skipped ones are named in the log (`... see-through skipped`, first 24). A transparent
renderer writing no depth used to be matched against the surface behind it, and where it passed within the tolerance in front of a
face it took the face's pixels.

**Guide depth** (`guideShader`): the depth before the transparents (event 5 in [frame-path.md](frame-path.md); if a frame has no such
copy, the depth after them), then `max` with the depth a person part matched (reversed Z: larger is nearer), then `max` with UI depth.

### The transparency rule

Two things move at once in a transparent pixel. If the transparent thing (a light beam, a haze layer, a glitter shell) is
given the pixel's motion and depth, the face behind it is dragged along with it, and frame generation shows the contour rippling. So:
transparent things carry no depth and no motion. VaM, however, draws a character's own see-through clothing (lace, a sheer
overlay) in the transparent queue (2501-2510) *and writes depth*, and that clothing must keep its own motion and depth, or the garment stops
being smooth. The implementation resolves both at once:

- The bridge hands over the depth from **before** the transparent pass (event 5, `CameraEvent.BeforeForwardAlpha`) and the compositor also keeps the
  depth after it, and the camera colour at both points (`opaqueColorCopy`, `fullColorCopy`).
- In `objectShader`'s pixel stage, a **person** (skin, clothing, mesh hair: `person.x = 1`) draw matches either depth. A fragment that matches
  the opaque depth owns the pixel (and, if it also matches the later depth, takes that depth). A fragment that matches **only** the later
  depth (it sits in the transparent queue) owns the pixel only if the transparent pass visibly changed it: `length(after - before) >= 0.25 *
  max(length(before), 0.05)` on the two colours.
- Props, CUAs and hair ribbons (`person.x = 0`) match only the depth before the transparents.

**Soft edges.** The 25% test is the answer to hair layers, hems, lace and cards that write depth but barely show: they would
otherwise take the pixel's depth and motion at their rim, and the outline flickered every frame because the pixels were mostly background
colour that moved with the hair. It covers all person parts, not only hair. Physics hair strands take only the opaque depth.
If the two colour copies are not available the compositor falls back to taking any later-depth match (`VAM DLSS see-through test: off` in the log).

**Parts the author hid.** A clothing part hidden with `Alpha Adjust = -1` on a see-through shader still draws in VaM (transparent queue,
depth written). The bridge (`Hidden()`, `VaMDLSSBridge.cs`) skips such submeshes when collecting skin and wrap draws: only for shaders whose name starts with
`Custom/Subsurface/` and contains `SeparateAlpha` (their alpha is `saturate(_AlphaTex.a + _AlphaAdjust)`, so -1 draws nothing; checked in the
disassembly). The path tracer's scene builder leaves them out of the scene too (`components/shading/skin/RtScene.cs`, same shader test). It is not valid for other shader
families: hair shaders (`Custom/Hair/*`) and opaque shaders put in that queue also carry an `_AlphaAdjust` property with a stray value and still draw;
the first version skipped any material in the transparent queue with the property and removed the hair's motion. Parts hidden by `_Color.a = 0` are not covered.

### Path tracing route

Under path tracing the compositor does not run the redraws for skin, hair or props (`pathOwnsMotion`: object motion, hair and rigid motion are off;
the stale histories are released when ownership flips, but the bone-row buffer for animated props is kept, see Pitfalls). The path tracer writes the primary
surface's depth, motion, instance and material per pixel (`PtPrimaryGuides` in `components/shading/rt-core/shaders/path_trace.hlsl`) from its own previous-frame geometry, and the
compositor merges UI on top. Transparency in the guides:

- **Layer walk** (up to 4 layers, front to back). Test bit 29 (`PT_TEST_OPAQUE_MOTION`): a see-through layer contributes no depth or motion; the guides take the
  surface behind every see-through layer (the opaque surface or the sky). Test bit 30 (`PT_TEST_PERSON_MOTION`): the first layer of a **person** keeps its depth
  and motion even if see-through: skin and `skin-wrap` draws (clothing, mesh hair) carry `PT_MODE_PERSON` (256), set in `components/shading/skin/native/rt.cpp`, and hair fibres count as
  people; see-through things in front of a person still do not. With the bits cleared the rules fall back to the deepest layer (29 cleared: the layer with the largest coverage).
- **Coverage guides.** Test bit 27 (`PT_TEST_COVER_GUIDES`): where coverage varies with the seed (sheer cloth, lace, translucent alpha), the guides (diffuse, specular, normal,
  roughness) average every layer in the share of frames its coverage gives, instead of only the layer above coverage 0.5; depth, motion, instance and material follow the layer with the
  largest share. A sheer layer under 0.5 used to be missing from the guides and RR resolved it away as if the cloth were clear.
- The test mask is read in the shader through `PtTest()` from the path-view constants; it is not a settings-file key, so changing a default means editing the code.
- **UI.** The bridge sends every visible UI rectangle (current and previous canvas transform) only while path tracing runs; the compositor draws them where they are in front
  of the scene and the guide pass merges their depth and motion. A flag marks elements as "on top"; ZTest Always shaders (`UI/Default-Overlay`,
  `GUI/Text Shader Overlay`, `Custom/TransparentHUD`, `Unlit/UnlitOverlayShader`) are on top, and in VR every world canvas is while path tracing runs.
- The RR stage also needs a "specular hit distance" and albedo, normal and roughness guides; those are not covered here: [denoiser.md](denoiser.md), [path-tracing.md](path-tracing.md).

### Principle: the character's motion wins

The character's motion is what the viewer looks at; an effect, a prop, a transparent layer or one of our own systems that pulls the character's pixels
off its motion is the bug. In practice:

1. a pixel's motion and depth come from the opaque surface it really shows (matching against the rendered depth does this);
2. transparent things never take it, except the person's own see-through layers;
3. UI is the one exception and stays on top of everything (menus pull their own depth and motion, or the frame goes native);
4. nothing in the pipeline may be implemented as a fixed draw priority of "people first": depth does the job, and a priority breaks the case where a prop really is in front of the face.

It applies equally to the physics components of the mod: they must not change what the animation data says the body does.

## Settings

| File / section / key | Default | Effect |
|---|---|---|
| `performance.ini` `[Performance] DLSSObjectMotion` | 1 | skin, clothing, mesh hair redraw (raster route) |
| `DLSSHairMotion` | 1 | physics hair ribbons (raster route) |
| `DLSSRigidMotion` | 1 | props, toys, CUA meshes and animated CUAs (needs `DLSSObjectMotion`) |
| `DLSSHairBias` | 0 | DLSS "bias current colour" mask value on hair pixels; 0 = off |
| `DLSSUIMotion` | ignored | removed; UI motion exists only under path tracing |
| `DLSSMenuNative` | 1 | native 100% frames, SR skipped, while VaM's own menus are open |
| `RRTransparentGuides` | 1 | legacy raster RR only; not reachable now (see [frame-path.md](frame-path.md)) |

The depth tolerances (1.5 cm / 3%, hair 2.5 cm / 2%) and the 25% visibility threshold are constants in `dx11_vam_dlss.cpp`. Skin tolerances must stay generous: VaM's skin shader
tessellates (Phong) and the rendered surface differs from the redrawn mesh by millimetres.

## Measured

- Not offline-testable: all rules above were checked by mechanism and by simulated-headset runs; a real headset or monitor is what shows ripple and shimmer.
- Unity's own motion vectors as an alternative (measured, see Tried and dropped): +0.02 ms on a heavy scene, +0.54 ms on a light scene, about +240 MB video memory; their values were right everywhere except hair.
- A library of 817 scenes: 49% of the scenes with clothed characters have at least one clothing material in a queue above 2500 (mostly layers over the face and eyes: eye shells, eye shadow, tear ducts); 7.2% have a shadow-casting directional light.

## Pitfalls

| Symptom | Cause | What to do |
|---|---|---|
| Face contour ripples like heat haze in generated frames, only some scenes | a transparent light, haze or card in front of the face took its motion/depth | check what draws in the transparent queue; the rule above |
| Sheer clothing is not smooth / takes the background's motion | clothing treated as transparent and given nothing | the person-part test and PT bit 30 |
| Hair outline pops or jumps | its see-through outer layer wrote depth and took hair motion | hair matches only the opaque depth; the 25% rule for parts |
| Hair lost its motion after a "hidden part" change | the hidden-part rule matched hair shaders | restrict to the `SeparateAlpha` family |
| Props vanish from motion after leaving view and returning, "Handle is not pinned" in the bridge log | cache sweep freed pinned arrays while the renderer entry still referenced them | a returning renderer is re-bound; anything pinned for native code must be known to the entries that use it |
| Crash on loading a scene with animated props under path tracing | the compositor released the bone-row buffer with the raster histories when path tracing took over motion | keep `objects.boneRows` (created once, rewritten whole every frame) |
| Props lose motion when the object is a `SkinnedMeshRenderer` | VaM lists only `MeshRenderer`s per atom | the animated-CUA collector |
| Hair strands vector wrong | wrong stride or point count (joining guides tip to root by `Segments`) | stride 56, count = guides x `TessSegments` |

## Tried and dropped

- **Unity's motion vectors for everything.** The bridge enabled them on the main camera only; the compositor merged Unity > redraw > camera ("Unity when it differs from the camera by
  more than half a pixel"). Values were correct (checked: no jitter, bottom-up v, transparents produce none). On the headset it produced strong artefacts, hair worst: GPUTools' hair shader has its own
  motion pass that takes the previous position from `current - TessRenderParticle.Velocity`, and in VaM that velocity is not a per-frame displacement, so Unity drew a wrong hair
  vector over the correct guide-strand one (about 4.7% of pixels on a head translation). Setting the hair renderers' `MotionVectorGenerationMode` to `Camera` would remove it; not done. A
  stable design would also need "Unity wins" to be replaced because wherever Unity is wrong, its wrong value wins. Reverted; do not propose it again without the hair fix.
  What Unity would add over the collected set: blend-shape skinned renderers, skinned renderers outside CUAs (a VR hand), and dynamically created renderers.
- **"People first" draw order** (skin and clothing, then hair, props only on pixels nobody claimed): installed, removed after minutes. Once transparents no longer wrote depth into the guide depth,
  the depth test alone was right, and the priority only broke props that really are in front of a face.
- **Giving hair an exception for its outline** instead of fixing the class (all person parts' soft edges).
- **Menu UI motion and a nasolabial-fold band** (raster route): removed; their scene scans caused periodic hitches in big scenes.
- **Skipping every transparent-queue renderer with `_AlphaAdjust`**: see "Parts the author hid".
- VaM's skin is drawn one frame later than the skeleton by design (`DAZCharacterRun.Update` runs `FinishRun` before `PrepRun`); this is self-consistent for motion vectors
  and is not a cause of errors.

## Open

- Transparent lights with motion of their own (beams, fog) are not given any; a pixel behind one keeps the surface's motion.
- Whether hiding parts by `Alpha Adjust -1` was the cause of an earlier face deformation was not confirmed.
- Menu text can swim on the raster route; a root-canvas-corner design exists but was not built.
- Parts hidden with `_Color.a = 0` still draw into the guide path.

## Verify

- Look at the logs: `opencomposite.log` lines `VAM DLSS scene depth: ...`, `VAM DLSS see-through test: ...`, `VAM DLSS hair motion active: N hair items ...`; the bridge lines `rigid motion for N moving MeshRenderer(s)`,
  `N see-through skipped`, `hidden clothing part ... not redrawn for motion`.
- With the simulated headset (`tools/lab/simxr/vrtest.py`): a head translation (`sim.motion(type="sine", axis="x", ...)`) plus a yaw turn, with the menu open and close, and
  hair filling the view, is what exposes wrong vectors; a still camera hides most of them. The path-tracing guides have offline GPU contracts in `components/shading/rt-core/tests/contract_probe.inl` (`coverage_guides`, `coverage_guides_off`,
  `person_layer_guides`, `person_layer_guides_off`; built with `build.cmd`, they need a D3D12 GPU with ray tracing).
- A real headset or the desktop window is the only place the ripple and shimmer artefacts can be judged; if one is reported, first ask which stage was switched off to isolate it (frame generation, SR, NR), and on which side of the contour it happens, and reason from the rules above before capturing images.
