# Dynamic Fur V2

Dynamic Fur V2 is an experimental SKSE plugin that builds fur shells at runtime
from keyword-selected live, skinned geometry on the player. The current milestone
renders a native 17-shell profile in third person and dynamically suppresses fur
under equipped armor without replacement meshes, texture rewriting or a custom
shader.

This is a development build, not a general-purpose release. Its renderer hooks,
body selection and lifecycle handling need in-game validation across supported
Skyrim runtimes and mod combinations.

## Current implementation

The project is divided into four modules:

- `DynamicFurCore` contains renderer-independent shell layout, alpha thresholds,
  LOD selection and geometry-based coverage calculations.
- `DynamicFurGenerator` is the pure CPU packed-vertex extrusion module; it has no
  Skyrim scene-graph, D3D-resource or hook dependency.
- `DynamicFurGame` resolves keyword-bearing ARMO sources, locates all of their
  active ARMA scene-graph parts, validates their skin bindings and creates an
  owned skinned carrier under the current third-person skeleton.
- `DynamicFurRender` builds immutable D3D11 sidecar resources, identifies supported
  Lighting and Utility passes and expands one carrier draw into multiple shell
  draws while restoring the original D3D11 state afterward.

`ShellVertexGenerator` is tested independently before its output is uploaded by
the D3D11 backend.

At runtime the controller currently:

1. Waits for Skyrim data and the player's third-person 3D.
2. Resolves `HasFur` from `TrueShell.esp|0x000800` and examines the 32 active
   editor biped slots. This is a bounded per-actor lookup, not a scan of every
   form loaded by the game.
3. For each active keyword-bearing `ARMO`, verifies that Skyrim's selected `ARMA`
   is declared in that ARMO's armor-addon list, deduplicates repeated biped slots,
   and accepts every usable skinned geometry beneath every distinct live part.
4. Clones those live geometries and their required render properties into a hidden,
   plugin-owned carrier. The source body geometry is not edited.
5. Reads the carrier vertex/index data and creates one immutable vertex buffer
   containing 17 copies. Each copy is displaced along its canonical shell offset,
   using packed vertex normals when present or reconstructing smooth normals once
   from indexed topology when the body stream omits them.
6. Reveals the carrier only after every accepted partition has a valid renderer
   sidecar.
7. Draws 17 layers in supported view passes and 4 layers in supported shadow
   passes. Each layer supplies its alpha-test reference through the vanilla
   16-byte pixel-shader constant-buffer contract at `b11`.
8. On armor equipment/3D changes, snapshots the live unmarked ARMA geometry,
   CPU-skins armor and carrier vertices into the current skeleton-root space and
   builds one reusable BVH for that equipment revision. Corners, shared edge
   midpoints, centroids and conservative swept bounds locate the local boundary.
9. On the observed 3BA SSE stream, adaptively subdivides only boundary triangles
   down to an 8-by-8 barycentric grid. New vertices interpolate position, UV,
   semantic extrusion normal and deterministic top-four global bone weights.
   They are CPU-skinned again and sampled in their exact packed pose before
   covered micro-triangles are removed from every view/depth/shadow shell.
10. Atomically swaps the completed immutable sidecars. Unsupported layouts or a
   refinement that exceeds the R16/memory budget fall back to the proven binary
   whole-triangle mask instead of publishing partial topology.

The current `0.4.7` profile has 17 shells and a total fur length of `1.0` model
unit. Its alpha thresholds are uniformly spaced from `0/255` through `240/255` in
steps of `15/255`; there is no unusable eighteenth `255/255` layer. The accepted
carrier passes override only depth with `LESS_EQUAL` and depth writes. Skyrim's
inherited blend state, independent MRT write masks, stencil, shaders, textures,
rasterizer and scissor state are preserved exactly; in particular, the observed
RGB-only `0x07` write mask is no longer replaced with RGBA `0x0F`.

When direct D3D draw routing is unavailable, the carrier's engine draw remains as
the zero-length scheduling shell at a `1/255` alpha-test reference and the
`RestoreGeometry` fallback adds layers 1 through 16. This keeps the Lighting pass
alive without drawing layer zero twice. Direct routing still replaces the source
draw and emits all 17 layers itself.

## Texture ownership

Every accepted geometry retains the diffuse authored for its active ARMA. Dynamic
Fur V2 does not open, copy, rewrite or replace those files, and it does not
substitute the `t0` diffuse binding. The active vertex shader, pixel shader and
texture bindings are preserved. Each geometry's existing diffuse alpha is
evaluated by the active alpha-test shader with a different threshold per shell.

`FemaleBodyFur.dds` is not loaded or referenced by the current implementation. It
is not required for installation. A loose file or BSA with that name has no effect
on this plugin unless another mod independently uses it.

Diffuse paths and geometry names are not source-selection contracts. A texture or
body replacer is eligible when its live skinned geometry belongs to an active ARMA
selected from a `HasFur` ARMO.

## Requirements

Runtime requirements:

- Windows x64 and Skyrim Special Edition or Anniversary Edition;
- a matching SKSE64 installation;
- Address Library for SKSE Plugins for the exact game runtime;
- the Microsoft Visual C++ 2015-2022 x64 runtime;
- a D3D11 feature-level 11 renderer;
- a compatible third-person skinned ARMA source selected by a `HasFur` ARMO.

Build requirements:

- Visual Studio 2022 with the Desktop development with C++ workload and the x64
  MSVC toolchain;
- CMake 3.24 or newer;
- Ninja;
- Git;
- vcpkg, with `VCPKG_ROOT` pointing to its repository root, for plugin builds.

Run the commands below from a Visual Studio Developer PowerShell or another shell
where CMake, Ninja and the x64 compiler are available.

## Get the source

CommonLibSSE-NG is a pinned Git submodule. Initialize the repository recursively:

```powershell
git clone --recurse-submodules <repository-url> DynamicFurV2
Set-Location DynamicFurV2
```

For an existing checkout:

```powershell
git submodule update --init --recursive
```

Do not use `git submodule update --remote` for a reproducible build; use the
submodule commit recorded by this repository.

## Build and test

### Core-only tests

The core-only preset does not build the SKSE plugin and does not require vcpkg:

```powershell
cmake --preset core-Debug
cmake --build --preset core-Debug
ctest --preset core-Debug
```

This runs `DynamicFur.Core` and `DynamicFur.Generator`. They cover profile
validation, canonical shell layout, LOD selection, coverage aggregation,
synthetic coverage stress cases and packed-vertex shell extrusion.

### Debug plugin and renderer tests

Set `VCPKG_ROOT` before using a plugin preset:

```powershell
$env:VCPKG_ROOT = 'C:\path\to\vcpkg'
cmake --preset x64-Debug
cmake --build --preset x64-Debug
ctest --preset x64-Debug
```

The full Debug preset builds the plugin plus `DynamicFur.Core`,
`DynamicFur.Generator` and `DynamicFur.Render`. The render test uses the Windows
D3D11 WARP device to verify shell-buffer upload, registry identity, per-layer
`b11` buffers and restoration of the original vertex-buffer and constant-buffer
bindings. It does not start Skyrim and does not validate the live hook chain.

The Debug DLL is written to:

```text
out\build\x64-Debug\DynamicFurV2.dll
```

### Release plugin

```powershell
$env:VCPKG_ROOT = 'C:\path\to\vcpkg'
cmake --preset x64-Release
cmake --build --preset x64-Release
ctest --preset x64-Release
```

The Release DLL is written to:

```text
out\build\x64-Release\DynamicFurV2.dll
```

There is currently no installer or packaged release target. Build output under
`out/` is local and is ignored by Git.

## Manual installation

Create a mod with this layout and copy the Release DLL into it:

```text
Dynamic Fur V2/
  TrueShell.esp
  SKSE/
    Plugins/
      DynamicFurV2.dll
      DynamicFur/
        armor_blacklist.json
```

Install that directory through a mod manager, or copy its contents into Skyrim's
`Data` directory so the final DLL path is:

```text
Data\SKSE\Plugins\DynamicFurV2.dll
```

No texture, mesh, INI or `FemaleBodyFur.dds` file is required by the current
build. `TrueShell.esp` is required and must remain enabled: it defines `HasFur`
at local FormID `0x800`. Add that keyword to each ARMO whose active ARMA geometry
should receive fur. The effective Skin ARMO is one source, but marked equipped
ARMO records are also eligible. Remove older prototype DLLs before testing so
only `DynamicFurV2.dll` owns this feature.

The blacklist is optional. To prevent a particular equipped ARMO from taking
part in dynamic coverage, create
`Data\SKSE\Plugins\DynamicFur\armor_blacklist.json`. The complete document is
just a JSON array of local Form specifications:

```json
[
  "MyCapeMod.esp|0x000D62",
  "AnotherCape.esl|0x000800"
]
```

The text before `|` is the exact plugin filename. The hexadecimal value after
it is the ARMO's plugin-local FormID, without the runtime load-order prefix.
Use an empty array (`[]`) to block nothing. Invalid or unresolved entries are
ignored individually and reported in `DynamicFurV2.log`; a missing file is also
valid and means an empty blacklist. Restart Skyrim after editing this file. A
copyable example is available at `config\armor_blacklist.example.json`.

Coverage also probes `0.5` model units inward from each carrier point. This
catches shoes and other tight armor whose rendered surface lies slightly below
the body mesh; it does not extend the normal outward reach used for loose cloth.

Normal configure, build and test commands never copy files into Skyrim or a mod
manager. Automatic deployment is disabled. The CMake project can expose a manual
`DynamicFurDeploy` target only when explicitly configured with both
`DYNAMICFUR_DEPLOY=ON` and an absolute `DYNAMICFUR_DEPLOY_ROOT`; release and debug
presets force deployment off.

### First in-game test

Use a disposable profile, remove the older Dynamic Fur prototype DLL, and start
in third person with the marked source ARMO parts visible. For a marked Skin ARMO,
the plugin must observe all of its declared biped parts once before it creates the
persistent carrier. Confirm that shell fur appears on every usable skinned
geometry belonging to its active ARMA parts.

Equip and unequip an ordinary ARMO after the carrier becomes visible. The log
should report the live armor triangle count and how many carrier triangles were
hidden. Fur should return when the piece is removed. `FemaleBodyFur.dds` still
does not change the result. If no shell effect appears, close the game and inspect
the log described below.

## Logs and diagnosis

The plugin writes `DynamicFurV2.log` to the log directory supplied by SKSE,
normally the game's `SKSE` directory under `Documents\My Games`. Logging is at
`info` level and flushes each informational message.

A successful startup should include messages similar to:

```text
Dynamic Fur 0.4.7 loaded; waiting for game data
Native shell backend ready: 17 view shells, 4 shadow shells, fur length 1.000
Carrier scheduling base is active: engine layer 0 uses threshold 1/255; shader-pass fallback adds only outer layers
Fur source policy ready: ARMO keyword 'HasFur' from TrueShell.esp|0x000800 ...
Armor blacklist ready: ... ARMO record(s) resolved from ...
Under-skin armor recovery is active: inward depth 0.500 model units
Player fur source state: eligible (skin 0x00069CE3, skin HasFur true, marked active ARMO 1)
Player carrier state: renderer-ready (...)
Carrier generation ... is visible through ... registered native partition(s)
Dynamic armor coverage revision ...: ... equipped ARMO, ... triangles ...
Adaptive armor boundary on ...: depth ..., ... source triangles refined, ...
Dynamic armor coverage hid ... carrier triangles on ...
First native carrier draw accepted via shader-pass fallback for pass 1: 17 shell(s), ...
```

Useful carrier states include:

- `waiting-for-3d`: the player skeleton is not available yet;
- `waiting-for-fur-source`: a marked Skin ARMO is not fully observable because
  one or more of its declared biped slots is unavailable or replaced;
- `source-rejected`: no acceptable source query/geometry was produced;
- `clone-rejected` or `build-rejected`: the live geometry could not be cloned or
  validated safely;
- `attached-hidden`: the carrier exists but renderer resources are not ready;
- `renderer-ready`: the carrier was registered and revealed;
- `detach-failed`: the old scene graph could not be detached safely.

Numeric `body`, `clone` and `binding` status values in the same log entry provide
the lower-level rejection category. Buffer readback and sidecar errors are logged
separately. A one-time `Native carrier draw suppressed safely` warning identifies
the first rejected pass in each category without logging every frame. When
reporting a problem, include the full log, exact Skyrim runtime, SKSE version,
Address Library version, GPU, and the list of renderer/body mods.

## Compatibility and current limitations

The build enables CommonLibSSE-NG support for Skyrim SE and AE and disables VR.
That is a build target, not a claim that every SE/AE executable or mod stack has
been validated. Match SKSE64 and Address Library to the exact executable and test
on a disposable profile first.

Current limitations are intentional and significant:

- only the player is managed;
- only third-person geometry is supported; first person is reserved for future
  work;
- source activation is actor-centric: it checks the effective Skin ARMO and the
  ARMO records currently represented in the 32 editor biped slots for
  `TrueShell.esp|0x000800` (`HasFur`);
- only active, race-selected ARMA instances have live scene-graph parts to clone;
  the plugin does not load every model declared by an inactive ARMO;
- when the persistent Skin ARMO is marked, all of its declared biped slots must be
  observable for the initial carrier build. An existing carrier is preserved
  while equipped armor temporarily replaces one of those source slots. A save
  first loaded already dressed can therefore remain at `waiting-for-fur-source`
  until the marked naked parts have been observable once;
- Core contains LOD selection and hysteresis, but runtime distance-based LOD is not
  yet connected; view passes currently request all 17 shells and supported shadow
  passes request 4;
- armor coverage remains binary with respect to shell ownership: a detected
  equipped surface disables every fur layer beneath that local patch. On the
  observed 3BA descriptor `0x0004300050000408`, however, the boundary is no longer
  limited to whole source triangles: corners, shared midpoints, centroids and a
  swept-AABB broad phase select local triangles for subdivision up to depth 3,
  followed by an exact re-skin/sample/compaction pass. This substantially reduces
  straight polygon cuts and small leaks while keeping collision work event-only;
- adaptive output is capped at 65,535 vertices and 262,144 candidate triangles.
  The generator retries shallower depths before reverting to the conservative
  whole-triangle mask. Other vertex descriptors currently use that fallback by
  design. A mathematically exact continuous boundary is still impossible with a
  finite polygon mesh;
- geometry whose live diffuse or exact geometry name matches a carrier is excluded
  from armor coverage so outfit NIFs containing an embedded body do not erase all
  exposed fur. Alpha cutouts in an armor diffuse are currently treated as solid
  geometry;
- HasFur ARMO parts are excluded from the first coverage policy to prevent
  self-occlusion. Cross-occlusion between multiple simultaneously furry garments
  is reserved for source-ownership tracking;
- coverage is recomputed on armor equipment and player NiNode/3D updates, not per
  animation frame. Ordinary shared-skeleton deformation remains aligned; large
  cloth/SMP motion can move beyond the event-time snapshot;
- only supported skinned vertex layouts and 16-bit index buffers are accepted.
  The refined path currently recognizes the live 3BA SSE layout (float position,
  half2 UV, half4 weights and four global ubyte bone indices); other valid carrier
  layouts continue to render and use binary armor coverage;
- the carrier needs an independent `NiAlphaProperty`. The plugin clones one from
  the source or loaded character graph when available; otherwise it creates a
  value-initialized property on Skyrim's heap and installs the runtime vtable
  before attaching it to the carrier;
- a pass must be an identified alpha-test Lighting or Utility output technique and
  expose the expected pixel shader plus 16-byte `b11` binding. Utility passes are
  classified by their real output flags because Skyrim's observed skinned depth
  technique `0x20B1` does not publish the generic `Texture/Skinned` flag pair;
  unmatched carrier passes are suppressed rather than drawn incorrectly;
- no custom shader is installed. The result depends on the active Skyrim shader
  honoring diffuse alpha and the vanilla alpha-reference contract;
- the plugin hooks the immediate D3D11 `DrawIndexed` and `DrawIndexedInstanced`
  entries and Lighting/Utility
  shader vtables. Compatibility with ENB, Community Shaders, shader replacements,
  capture/overlay software or another plugin hooking the same entries is not
  guaranteed;
- there is no in-game configuration UI, persistence format, installer, automatic
  packaging or automatic deployment;
- automated tests cover Core and isolated D3D11 resource/state behavior, not a
  real Skyrim render frame, save/load cycle, race change or long-session stability.

Save/load recovery listens to `PreLoadGame`, `PostLoadGame`, player 3D lifecycle
events and the closing of Skyrim's Loading Menu. The redundant recovery edge is
intentional because `PostLoadGame` can occur before the replacement biped graph is
fully observable, while a late unloaded event may otherwise leave rendering
suspended.

The safest failure mode is deliberate: a carrier remains hidden or its matched
unsupported pass is suppressed when validation fails. Treat a successful build
and passing unit tests as prerequisites for in-game testing, not proof of runtime
compatibility.

## Repository layout

```text
include/DynamicFur/Core/    shell profiles, math, LOD and coverage contracts
include/DynamicFur/Generator/ pure CPU packed shell extrusion
include/DynamicFur/Game/    keyword/ARMA lookup, skin validation and carrier ownership
include/DynamicFur/Render/  D3D11 sidecars, routing, state scopes and hook contracts
src/Core/                   renderer-independent implementations
src/Generator/              standalone packed shell generator
src/Game/                   Skyrim scene-graph implementations
src/Render/                 native multi-draw backend and hooks
tests/CoreTests.cpp         renderer-independent unit/stress tests
tests/GeneratorTests.cpp    packed-vertex generation tests
tests/RenderTests.cpp       isolated D3D11 WARP tests
lib/commonlibsse-ng/        pinned CommonLibSSE-NG submodule
```

## License

See [LICENSE](LICENSE).
