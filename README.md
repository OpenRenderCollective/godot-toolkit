# Remix Studio

A standalone Godot application for authoring RTX Remix replacements in OpenUSD. The document stays in USD; a separate Remix or OpenRemix worker renders the viewport. It does not use Omniverse Kit.

This is an initial implementation, not full Toolkit parity. The embedded Vulkan viewport targets Linux x86_64 and, with a Remix Plus runtime, Windows x64. Other Windows runtimes use a separate preview window.

## Build and run

On Linux, install a C++ toolchain with Clang, Python 3.12+, Git, a Vulkan loader/driver, and the normal Godot X11/Wayland development dependencies. Use a Vulkan ray-tracing capable GPU. Development and GPU testing were performed on an RTX 2080.

```sh
python scripts/bootstrap.py all --jobs 8 --openremix-source /path/to/openremix
python scripts/bootstrap.py run
```

The first build downloads and compiles OpenUSD 25.11, oneTBB, Assimp, Godot, godot-cpp, and the Remix SDK/runtime. It requires substantial disk space and build time. Everything goes under `.work`, `build`, and `app/bin`. Existing reference checkouts under `ref` are used as clone sources and are not modified. Revisions and source archive checksums are in `dependencies.lock.json`; engine/runtime changes are reproducible patches in `patches`.

Individual build targets are `deps`, `godot`, `remix`, `openremix`, and `editor`. The editor uses the locally built runtime automatically, or the library named by `REMIX_RUNTIME_PATH`. `REMIX_RENDERER_WORKER` overrides the worker executable.

To launch the already built app directly:

```sh
.work/deps/godot/bin/godot.linuxbsd.editor.x86_64.llvm --path app
```

### OpenRemix on Linux

Use an OpenRemix checkout containing `remix_editor_ext.h` and the optional USD loader:

```sh
python scripts/bootstrap.py openremix --openremix-source /path/to/openremix
# Or register an independently built runtime:
python scripts/bootstrap.py openremix --openremix-library /path/to/libremix.so
```

`OPENREMIX_SOURCE_DIR` and `OPENREMIX_RUNTIME_PATH` provide equivalent overrides.
CMake accepts `OPENREMIX_SOURCE_DIR` and `OPENREMIX_LIBRARY`. Development runtime
paths are written under `build/`; portable launchers resolve both runtimes locally.
The Linux build enables OpenUSD 25.11. Its Clang flags match the toolkit's GCC-built
OpenUSD/TBB ABI. The renderer's USD parser is CPU-only and remains optional for
standalone OpenRemix builds.

Render settings are saved separately for each renderer. Existing settings migrate
to Remix. OpenRemix offers path tracing, unlit raster, DLSS/FSR, quality, Ray
Reconstruction, and resolution, plus tabs for denoising, sampling, lighting,
SHARC, and display transforms. Effective fallbacks never replace saved choices.
REBLUR SH, SIGMA, and SHARC default on. The additional **PBR firefly filter
(REBLUR)** defaults off; ordinary NRD anti-firefly processing is already enabled.
The dialog reports live NRD/SIGMA/TAA, firefly filter, and SHARC status, including
an enabled cache with no eligible work. RR replaces NRD/SIGMA while preserving
their saved settings; turning RR off restores them. Progressive reference
rendering bypasses temporal filters, reconstruction, and SHARC. Direct/indirect
clamps use linear luminance, with zero disabling clamping; nonzero values can
remove real highlights and bypass lighting reuse. Advanced changes apply in the
existing worker. Settings remain separate from OpenRemix game profiles.
Frozen snapshot meshes are static SHARC receivers. A mesh being transformed in
a preview is temporarily dynamic; cancelling or committing returns it to the
static scene so caching can resume.
The embedded OpenRemix worker disables game profiles and automatic mods because
it receives the toolkit's already-composed USD snapshot. Authored lighting controls
the preview, without a fallback sun or fixed local-light range. Material rendering
retains OpenRemix's documented approximations.

For a bundle, supply the same `--openremix-source` and optionally
`--openremix-library` to `bootstrap.py portable`. Each runtime has its own directory,
dependencies, and license notices. `test_portable.py --gpu` checks both renderers
with the source checkouts hidden. `app/tests/openremix_smoke.gd` checks shared GPU
frames, resolution changes, recovery, and renderer switching. The optional native
`openremix_parity_tests` target compares the direct loader with toolkit composition,
and `openremix_mod_replay_tests` replays a toolkit-exported mod against captured hashes.
`openremix_project_smoke.gd` loads a project read-only and renders its captured camera.
`openremix_settings_smoke.gd` checks live NRD/firefly/SHARC controls and the
transition between NRD and RR without restarting the worker.
The OpenRemix GPU smoke test enables Godot's validation layer and aborts on API errors;
install the Vulkan validation layers and enable `REMIX_VULKAN_VALIDATION=1` and
`VK_LAYER_VALIDATE_SYNC=1` to validate the worker and shared-frame synchronization too.

## Portable Linux build

After building the application, run `python scripts/bootstrap.py portable`. This creates
`dist/RemixStudio-linux-x86_64.tar.gz` and its SHA-256 checksum. Extract the archive
to a writable folder and run `./remix-studio`; no installation or build tools are needed.
The package includes the patched Godot engine, GDExtension, Remix worker/runtime,
DLSS libraries, USD libraries and schemas, Assimp, oneTBB, and MDL materials.
Godot retains its editor capabilities for the authoring image codecs/compressors.

The package needs Linux x86_64, an X11/Wayland desktop, a Vulkan loader and a
Remix-capable GPU driver. It uses the build host's glibc baseline, recorded in
`build-info.json` and checked by the launcher; the current build needs **glibc 2.44+**.
It does not bundle glibc or GPU drivers. Build on an older compatible toolchain/sysroot
to target older distributions. This is a Linux toolkit package; the game's Windows
`d3d9.dll` is separate.

Settings, recent projects, logs and caches live in the bundle's `data/` directory.
`REMIX_STUDIO_DATA_DIR=/writable/path ./remix-studio` relocates that data when needed.
Existing renderer/settings environment overrides remain supported. CLI access is
available with `./remix-studio --cli inspect /path/to/project.usda`.

Packaging uses an explicit application-file allowlist and a fresh extension registry;
it excludes local projects, captures, screenshots, tests, settings and import caches.
Projects remain external and must retain access to their captures/assets. Shared
libraries use relative runtime paths, and USD keeps its plugin/schema layout.
Component notices, source patches, build provenance and file checksums are included.
For custom output names/locations, run `python scripts/package_portable.py --help`.

Run `python scripts/test_portable.py --gpu` to extract and validate the archive in
a path containing spaces, with the source checkout hidden by Bubblewrap. The test
covers USD loading, BC7/MDL ingestion, project reopening, CLI access, relocation,
separate writable settings and actual Remix viewport presentation. Omit `--gpu`
for headless checks. Test logs/screenshots remain in the reported temporary folder.

## Workflow

1. The welcome screen offers your recent projects with previews from their last successful Save. Choose **New from capture**, select a Remix capture, and create the project in a new directory. Or choose **Open** for an existing Toolkit workfile.
2. The selected renderer starts automatically when you open or create a project with **Render** enabled. **Renderer** selects Remix (the existing default) or OpenRemix (path tracing by default). Switching restarts the worker and retains the composed scene, camera, selection, and unsaved edits. Turning it off pauses rendering and shows the Layout view; turning it on starts or resumes Remix. Toggle it off and on to retry after a renderer failure. Initial shader compilation can take tens of seconds. The camera defaults to **Drone**: press **Z** over the viewport to capture the mouse, look around with the mouse, and fly with **WASD**. **Q/E** moves down/up, **Shift** moves faster, and the wheel adjusts flight speed. Press **Z** or **Escape** to release the cursor. While released, middle/right drag looks around and the wheel zooms. Choose **Orbit** for orbit controls; **F** frames the selection in either mode. Controls work in both Layout and Remix views.
3. **Add light** creates a light at the nearest surface in the centre of the view, with space for the emitter outside the surface. Area and spot lights face the surface. To place a mesh, ingest it, select it in the asset library, and choose **Add mesh**. Both start unattached. Use **Parent → Pick parent in viewport** in Properties, hover a mesh to highlight it, and click to attach. Escape or right-click cancels; **Detach** returns the object to the world. The picker works over the live Remix image and Layout view; middle-drag and the wheel still move the camera. Changing parent preserves the object's world pose; duplicates inherit the same parent and local transform. Picking captured geometry attaches to its stable mesh-hash root rather than the capture-only mesh child. **Replace mesh** and **Replace material** still use the selected captured asset.
4. Select a scene object to use its grouped authoring controls: transforms and bound materials for meshes; radiance, color, temperature, shape, and cone controls for lights; texture channels and surface settings for materials. Commit numeric edits with Enter, focus loss, or release of a drag; use ↺ to reset an active-layer property override. Choose **Move** or **Rotate** above the viewport and drag the colored world axes or rotation rings. The center square moves in the view plane. **Snap** uses whole scene units and 15° increments; **Escape** or right-click cancels a drag. Light guides show the physical sphere, rectangle, disk, or cylinder, with emission arrows and spotlight cones including their soft edge. Distant and environment lights use orientation symbols. Guides and transform handles remain visible during Drone flight; release the cursor to manipulate them. Gizmos work in Layout and Remix views, preview live, and commit one undo step on release. Edits and attachments to captured assets affect every instance of the same hash.
5. To restore a captured mesh, select its replacement in the viewport or scene tree and choose **Revert mesh replacement** in Properties. This restores matching captured instances, keeps added meshes and lights, and supports Undo/Redo. Lights or meshes parented inside the replacement move back under the captured mesh without changing their world pose. The ingested asset remains in the library.
6. Save the project, or choose **Package mod** to create a new, self-contained replacement mod directory containing `mod.usda` and its dependencies.

Choose **Link to game…** in the toolbar, select the game folder (or its `rtx-remix/mods` folder), and enter a mod folder name. This creates `rtx-remix/mods/<name>` as a symlink to the project's live mod. Existing mod folders and unrelated symlinks are never replaced. Enable the mod in the game's Remix menu if necessary; the runtime's hot reload applies updates after its next file check.

While linked, committed edits and Undo/Redo publish automatically after a short debounce; Save also publishes immediately. Drag previews publish when confirmed. The **Game linked** button opens the link controls: **Sync now** and **Unlink**. Link errors appear on the button and in the dialog. The link survives restarting the editor and reopening the project, which republishes the loaded project state. Unlink removes only the game symlink and its remembered settings.

Light and mesh attachments use explicit USD translate, rotateXYZ and scale operations, with matrices retained when needed to preserve shear. Light bounds follow their emitter dimensions. Live publication and packaging also normalize static matrix attachments from older projects and add replacement-layer game, timing and coordinate metadata. This preserves the authored position and size.

When installed mods already replace an attached mesh/light hash, live publishing automatically retains the first matching mod's attachments beneath the project's edits. Remix's first-match lookup must see the combined entry, so the editor gives its own symlink an earlier name when necessary. Refresh the game's mod list or restart it once after that change; the link dialog reports the new path and retained mods. The previous owned link remains valid until **Unlink** for running file watchers. Other mods' files are never edited, and unrelated replacement hashes keep their original behavior. Inherited mesh material bindings and dependencies are retained with their source layout. The editor viewport and portable **Package mod** remain project-only; automatic retention applies to the linked game's generated mod.

Live publishing includes unsaved replacement opinions, excludes captures, and links textures and MDL dependencies without copying their data. It leaves project saving and Undo history unchanged. The game keeps the last published state when the editor closes, including unsaved edits; Save is still required to retain those edits in the project. Generated USD and asset links live in `<project>.studio-live/`; `<project>.studio-link.json` remembers the destination. Keep the project and source assets in place while linked. After unlinking, the live cache may be deleted. Use **Package mod** for a portable copy. On Windows, creating symlinks requires Developer Mode or suitable permissions.

Use **Add capture** above the scene tree to attach another Remix capture to the open project. The **Capture** picker switches the active scene; hover an entry to see its full path. Only one capture composes at a time. Replacements and ingested assets stay shared, pending inspector edits are committed before switching, and each capture remembers its own camera. Adding and switching captures support Undo/Redo. **Save** persists the capture list and active capture in the workfile; capture files remain external, unchanged references. Missing captures stay listed but cannot be selected. Packaged mods exclude both capture geometry and the editor capture list.

The Remix viewport automatically reads mesh categories from the capture and the nearest
`rtx.conf` found by walking up from the capture's directory (including the usual
`<game>/rtx-remix/captures` layout). It reads only category hash lists, including decals,
terrain, particles, water, sky, and alpha overrides. Other game render settings do not
replace the toolkit's settings. The status output reports the configuration source.
Both legacy `remix_config` and namespaced `remix:remix_config` embedded capture settings
are supported, so captures moved away from their game still retain their saved categories.

Texture lists match the captured albedo texture hash; `rtx.skyBoxGeometries` matches
mesh hashes. As in Remix, positive hashes add categories and `-0x...` entries remove
them from inherited settings; an empty list adds no overrides. The game's file takes
precedence for hashes it mentions, and explicit USD category edits in project layers
take precedence over both. Material replacements retain their original captured texture
identity. Different material subsets can carry different categories. Switching captures
or reopening a project refreshes its configuration; a full scene refresh also rereads the
file. This does not modify the game configuration, capture, or published mod.
Categories are passed through the public Remix API; effects that require the original
game's raster passes, such as terrain texture baking, remain limited by the captured data.

**Ctrl+T** places the selected object on the surface under the mouse, preserving rotation, scale, and parent attachment. With the mouse captured for Drone mode, it uses the centre crosshair. The moved object's own mesh is excluded from the depth test; meshes rest on their bounds and lights clear their emitter dimensions. Placement is one undo step. If no mesh is hit, placement tries the stage's ground plane; if that also misses, Ctrl+T leaves the object unchanged and Add light falls back to the camera focus distance.

With the cursor released over the viewport, **G** moves the selection in the view plane, **R** rotates around the view axis, and **S** scales uniformly. Move the mouse to preview; **X**, **Y**, or **Z** constrains the operation to that world axis (press the same axis again to release the constraint). **Shift+T** aims a selected light's local −Z direction at the surface under the cursor, keeping its position and scale. Aiming uses captured geometry depth, with the ground plane as a fallback; missing a surface keeps the last valid aim. **Left click** or **Enter** confirms one undo step; **Escape**, **right click**, or losing window focus cancels. Snap applies whole-unit movement, 15° rotation, and 0.1 scale increments. These shortcuts work for lights and meshes in Layout and Remix; typing in properties and captured Drone flight retain their normal controls.

For precise rotation, press **R → X/Y/Z**, then type an angle in degrees, such as **45** or **−22.5**. The typed angle rotates from the pose at the start of the operation and overrides mouse movement and snapping. Negative values, decimals, and the numpad are supported. **Backspace** edits the number; erasing it returns to mouse control. **Enter** or **left click** confirms, and **Escape** or **right click** cancels.

New lights match the defaults in Toolkit's `omni.flux.light_creator.widget`: sphere radius 5 m / radiance 100, rectangle 10 × 10 m / 400, disk radius 5 m / 500, cylinder radius 5 m and length 10 m / 140, and distant angular size 1° / 25. Sizes are converted to the stage's units. Spotlights use the sphere defaults with a 35° cone; domes use USD's intensity default. Existing lights retain their settings. Radius describes the physical emitter, rather than its illumination range; an emitter intersecting the surrounding geometry can lose illumination. Cone and arrow lengths are display guides for direction and angle, not illumination cutoffs.

The editor remembers each project's camera on Save, when switching projects, and on a confirmed close—even when unsaved scene edits are discarded. It restores the camera position, orientation, Drone/Orbit mode, orbit pivot, and flight speed when you reopen the project, with the mouse unlocked. Missing or invalid view data falls back to Frame all.

Camera metadata and previews live beside the workfile as `<project>.studio.json` and `<project>.studio.png`. Copy these with the project to keep its view. Only a successful Save updates the preview; closing preserves the last-save image. Previews use the current Remix frame when available, or a separate Layout view without starting Remix. The sidecars are excluded from mod packages. Recent-project history is local to the application user directory and retains up to 20 projects; **Remove** only forgets an entry. **Recent projects** returns to the welcome screen, and `--project=/path/to/project.usda` opens directly into a project.

The compact interface uses system DPI scaling. Resizing the window changes workspace space without enlarging text and controls; display-scale changes update automatically. Toolbars wrap when needed, and dock splits remain resizable.

The capture stays unchanged. Undo/redo covers authoring-layer edits and capture changes in the workfile, including rollback of failed compound edits. Packaging includes unsaved authored changes, preserves nested replacement layers, excludes the capture sublayer, and refuses to overwrite an existing destination.

Capture texture paths are resolved relative to the USD layer that authored them, including referenced material layers. Windows-style separators are supported when previewing captures on Linux. Missing textures are reported in the editor log; resolving preview paths does not rewrite the capture.

The Remix viewport preserves material filtering and U/V wrap modes. Materials without these settings use Remix's linear filtering and repeat defaults, so terrain and other tiled textures sample correctly outside the 0–1 UV range.

Capture transparency uses each mesh instance's alpha-test, blend, texture-factor, and vertex-opacity data, including shared meshes drawn with different transparency states. In a material's **Opacity** group, disable **Use captured alpha state** to author overrides. For cutouts, choose **Greater** (or **Greater or equal**) and a threshold from 0–255. For smooth transparency, turn on **Enable blending**, choose **Alpha**, and leave alpha testing at **Always (no cutout)**. Remix's additive, multiply, color, and reverse/inverted blend modes are also available. Textured materials read opacity from the base-color texture's alpha channel; **Opacity** is the fallback for materials without that texture. Meshes also expose **Ignore alpha channel** and **Force cutout**. Changes support Save, Undo, Redo, and Reset. glTF MASK and BLEND imports author the matching material settings. Asset thumbnails approximate common cutout/blend modes; the Remix viewport is the rendering reference.

**Render settings** above the viewport controls upscaling, quality, Ray Reconstruction, and output resolution. **Automatic** chooses DLSS when supported, otherwise FSR for OpenRemix (or native resolution if unavailable) and TAAU for Remix; **Off** renders at native resolution. Quality presets control the internal render resolution. Ray Reconstruction is shown only when the active runtime reports support, and requires DLSS. Unavailable upscalers are disabled (the pinned Linux build does not include NIS). Saved unsupported selections use the renderer’s available fallback while retaining the preference. Output resolution offers presets from 960×540 through 4K and custom dimensions; it is independent of UI DPI scaling. Apply saves preferences across all projects and app restarts in `render_settings.json` beside the recent-project history. Upscaler changes apply live; resolution changes reconnect the viewport while retaining the document and camera. Cancel discards unapplied changes.

The build/run helper places NVIDIA's downloaded NGX feature libraries beside `libremix.so` so DLSS and Ray Reconstruction can be discovered. An externally supplied runtime must include its corresponding feature libraries; capability detection also checks hardware and driver support.

The resizable bottom asset library shows mesh previews and material spheres. Use **New folder**, **Rename**, **Move to…**, or drag assets/folders onto a folder to organize the library. Search finds assets across all folders; the type filter separates meshes and materials. Removing a folder moves its contents to its parent. Imports go into the folder selected when import options open.

Drag one or more model/image files from your file manager into the toolkit window
to ingest them into the current project's selected asset folder. The import dialog
lets you adjust model scale, image material type/texture role, and normal convention
before starting. Multiple files share those options and import one at a time;
existing asset names receive numbered suffixes. **Ingest asset** also supports
multiple selection. Unsupported files and folders are skipped, and a failed file
does not stop the rest of the batch. **Cancel import** cancels the active job and
remaining queue; switching projects discards pending imports. New assets receive
the usual library entries and thumbnails.

Folders and display names are virtual: asset files and USD references stay in place. Organization autosaves to `assets/.library.json`, independently of scene Save and Undo. Existing ingested assets are discovered automatically, missing assets remain marked, and a damaged catalog is preserved with organization made read-only. Selecting a library asset shows read-only details; Replace uses the last selected scene object and bound material slot, while Add mesh spawns a standalone object.

Thumbnails use a separate Godot preview scene with approximate material shading. USD extraction runs in the background, one preview renders at a time, and visible assets get priority. The 256px previews are cached in `assets/.thumbnails` and invalidated when their dependencies change. **Refresh** discovers new files and checks cached previews; **Retry preview** retries selected assets. The catalog and thumbnail cache are excluded from mod packages.

## Implemented scope

| Area | Support |
| --- | --- |
| USD documents | `.usd`, `.usda`, `.usdc`; workfile sublayers; authoring layer selection; captured hash references; save/reopen; dependency remapping through the native command API |
| Static mesh ingestion | USD assets with a default prim; glTF/GLB, FBX, OBJ through Assimp; transforms, normals, UVs, material assignments; unit/axis normalization |
| Captured skinned meshes | Referenced USD skeletons, per-instance captured poses, rest-pose fallback, joint weights and bind transforms; posed geometry and normals in both viewports, picking and framing |
| Materials | AperturePBR opaque/cutout and translucent parameters; material replacement; PNG/JPEG/TGA/EXR/HDR conversion and existing DDS assets |
| Texture processing | BC7 color, BC6H HDR, BC5 hemispherical-octahedral normals, BC4 scalar maps, mipmaps, glTF packed metallic/roughness extraction |
| Authoring UI | Curated typed mesh/material/light controls; bound material slots; active-layer reset; virtual asset folders, search, and cached thumbnails |
| Placement | Standalone or attached meshes and sphere/spot/rect/disk/cylinder/distant/dome lights; parent picker preserving world pose; move/rotate gizmos, radius guides, snapping; duplicate/delete; visibility via properties |
| Viewport | Composed USD triangles, material subsets, material/light updates, camera orbit, nearest-triangle picking, selection outline, wireframe layout mode |
| Renderer isolation | Framed IPC; GPU memory sharing on Linux; synchronized GPU texture copies; manual worker restart without discarding the document |

Captured skinned meshes automatically resolve the skeleton assets referenced by the capture (normally in its sibling `skeletons/` directory). Keep that directory with the capture's other dependencies. Each instance displays its captured skeletal pose; time-sampled captures display the first sample. Evaluation changes only viewport geometry, leaving skeletons, weights, poses, and source mesh data intact when saving or publishing. Missing or invalid skeleton data produces a status warning and displays the undeformed mesh.

Selecting a skinned mesh or its instance shows its bones over the model in both Remix and Layout views: blue joint connections and markers, with gold root joints. The overlay follows the captured pose, stays visible during Drone flight, and clears when the mesh is deselected or hidden. It is a visual guide; selecting and editing individual bones is not supported.

Unsupported USD content is retained when opening and saving a project. The preview is a static rendering adapter: animation playback, bone/weight authoring, blend shapes, graphs, particles, arbitrary MDL networks, terrain authoring, and Toolkit-specific advanced workflows are outside this implementation. FBX/glTF ingestion remains static; imported non-Remix USD shaders are not automatically converted into complete AperturePBR networks.

## Architecture and limits

`native/document.cpp` owns the OpenUSD stage, layers, edit transactions, and packaging. `native/ingestion.cpp` converts source assets into staged USD assets before publishing them. The GDExtension exposes this to the standalone UI in `app/scripts` and runs ingestion on a background thread. Command results pass directly into Godot dictionaries and arrays, avoiding a full JSON string and Godot's signed 32-bit string/parser limit when opening large captures. Texture conversion uses Godot's codecs and compressors.

`native/scene.cpp` composes visible geometry and lighting into a renderer snapshot. `native/renderer_main.cpp` creates Remix SDK materials/meshes/lights and submits instances each frame. Snapshots list each distinct geometry once under `prototypes`, as indexed float32/int32 buffers grouped into per-material surfaces; `meshes` are instances that name a prototype and carry their own transform, alpha state, category flags per surface and skeleton. Instances share a prototype when their points, topology, primvars, orientation, skinned pose and material bindings are identical. Instances of a prototype with equal category groups share a Remix mesh. Light edits send a small lighting snapshot and update only changed lights, preserving Remix mesh/material resources and the Layout geometry/picking cache. This includes light creation, deletion, and undo/redo; edits to lights with geometry descendants use the full scene path. Editor lights remain dynamic so repeated edits keep updating their illumination. Invalid captured UV components use zero in the preview without changing the USD assets. Geometry changes send complete snapshots in bounded IPC frames without constructing a single scene-sized string. Transfers can exceed both 64 MiB and 2 GiB. Protocol v3 uses the high bit of each little-endian length header to mark continuation frames. A message is a chain of JSON text in which binary buffers appear as `{"$buffer": index, "type": name}`; when its top-level `buffers` array lists their byte lengths, a second chain carries the concatenated buffers. A message is decoded and applied only after its final frame arrives. The editor and worker must be updated together. Snapshots still need to fit in memory, and every geometry edit still resends the whole snapshot.

The layout view uploads each unique mesh to Godot once per scene snapshot and draws wireframes on the GPU. It caches the resulting viewport image until the camera, selection, scene, or view size changes. Repeated instances share GPU geometry and a picking BVH, built lazily on first use. Selection first rejects mesh bounding boxes, then queries the native triangle BVH; framing uses cached world bounds. Large snapshots still take time to compose and upload when the scene changes, but orbiting does not walk their triangles in GDScript.

The Linux runtime patch exports an offscreen Vulkan image and two binary semaphores. Handles travel over a private Unix socket using `SCM_RIGHTS`; ordinary control messages use length-prefixed stdio. Godot verifies the GPU UUID, imports the allocation, and copies it into an engine-owned texture with queue ownership transfers and semaphore waits/signals. No CPU image readback is used for viewport presentation. The worker completes its GPU submission before acknowledging a frame so a crashed producer cannot leave Godot waiting for an unsignaled future frame. Frame production is serialized, with a configurable output image (960×540 by default) scaled to fit the viewport.

The preview uses Remix's RIS light sampling. The pinned native fork produced black analytical lighting with RTXDI enabled in the embedded viewport; RTXDI is disabled until that integration issue is resolved.

The editor retains the runtime's automatic quality settings in Custom mode and enables Remix's stochastic alpha compositor. This lets viewport settings take precedence over the automatic Low preset, which otherwise disables regular surface transparency.

The runtime material-lifetime patch keeps per-instance material overrides alive through surface creation. Captured additive/emissive blends otherwise leave dangling texture references and can crash the renderer while loading a scene.

If a colored light appears white in a linked game while its published USD has the correct `color3f inputs:color`, check the game's runtime build. The `sambow23/dxvk-remix-gmod` build `12759b3e` checks USD colors against the renderer's private `Vector3` type; USD stores them as `GfVec3f`, so the loader rejects them and keeps white. The editor viewport sends light colors through the Remix API and does not use this broken USD loader. Changing the exported property name or its USD vector type cannot fix that runtime check.

[`patches/remix-usd-light-color.patch`](patches/remix-usd-light-color.patch) fixes the conversion while retaining type validation for malformed values. It applies to both the pinned reference and `12759b3e`. Bootstrap applies it to the local runtime source, but this does **not** update a game's Windows `d3d9.dll`. Apply the patch to the matching game runtime source checkout with `git apply /path/to/godot-toolkit/patches/remix-usd-light-color.patch`, then build and install that game's runtime. The `12759b3e` patch has been built as a release x64 DLL with Visual Studio 2022: Windows DLL loading, D3D9/Remix API exports, and color conversion using the runtime's stock USD 25.11 libraries passed. In-game rendering remains unverified.

For that revision, from an x64 Visual Studio Developer PowerShell with the runtime's prerequisites installed:

```powershell
meson setup _Comp64Release --buildtype release --backend ninja -Denable_tracy=false "-Dcpp_link_args=['/IGNORE:4099','/IGNORE:4199']"
meson compile -C _Comp64Release d3d9
meson install -C _Comp64Release --no-rebuild --tags output
```

This stages the runtime in `_output`. The two narrow linker exclusions handle missing vendor PDB files and an unused delay-load declaration; other warnings remain errors. Build the `d3d9` target explicitly: the optional Hydra demo application at this revision has a separate `LNK4098` C-runtime conflict.

The embedded Windows viewport needs a runtime that exports its output image through `remixapi_ConfigureExternalFrame`, which [Remix Plus](https://github.com/Kim2091/dxvk-remix) provides from rc2. Build the renderer worker against that runtime's API headers:

```powershell
cmake -B build -DREMIX_API_INCLUDE_DIR=/path/to/remix-plus/public/include -DREMIX_PLUS_API=ON
```

The runtime shares the image and two semaphores with the editor as Vulkan external memory (Win32 NT handles). With Remix Plus, the worker also draws the scene through retained instances, one call per frame. Other Windows runtimes, including OpenRemix, fall back to a separate native preview window until they provide an equivalent export. The portable application bundle currently targets Linux only.

## Validation

```sh
python scripts/bootstrap.py test
python scripts/bootstrap.py gpu-test
```

Native tests cover capture immutability, composition, shared instance edits, transaction rollback, undo/redo, material and mesh replacement, and packaging. Protocol tests cover normal sessions, invalid versions, truncated continuations, oversized frames, UTF-8 across frame boundaries, transfers and replies above 64 MiB, and recoverable command errors. A headless Godot test exercises large transfers through the real client and worker. The Godot ingestion test checks DDS formats, mipmaps, channel values, glTF ingestion, and material dependency packaging. The GPU test checks visible rendered geometry, invalid captured UV handling, and worker restart while retaining unsaved edits; it needs an active desktop session and supported GPU. Test screenshots and generated projects are retained under `.work/integration` and `.work/ingestion`.

`large_scene_test.gd` checks direct native scene transfer, Unicode paths, bounded JSON encoding, cancellation, and UI recovery after a failed scene response. To also load a real large project, build its Layout geometry, and stream the complete snapshot to the worker without allocating Remix GPU geometry, run:

```sh
.work/deps/godot/bin/godot.linuxbsd.editor.x86_64.llvm --headless --path app --script res://tests/large_scene_test.gd -- --project=/absolute/path/to/project.usda
```

The optional project is opened read-only; this regression can require substantial memory. Its generated test fixtures are retained under `.work/large-scene`.

Category tests cover game-config discovery, embedded settings, hash additions/exclusions,
USD overrides, material subsets, capture switching, and read-only source handling.
The category GPU smoke test checks visible results of changing a hash classification,
including a capture whose meshes are all hidden. Its images are in `.work/categories-gpu`.

Model import tests cover FBX texture paths with Windows separators, spaces and
absolute paths, plus missing-texture cleanup. Added and replaced meshes retain
their imported root transforms through project unit/axis conversion. Replacement
selection and gizmo edits use the visible capture instance while authoring shared
hash replacements; tests cover transformed parents, undo/redo, reopening and export.
The Godot ingestion smoke test also accepts `-- --fbx=/path/to/model.fbx` to check
real FBX files without modifying them.

Layout tests cover nearest-hit picking with transformed instances, shared geometry, cache invalidation, GPU wireframe visibility, selection, resizing, and alignment of the transparent selection overlay with the Remix view.

Game-link tests cover relative USD dependencies, linked textures and MDL bundles, nested layers and offsets, unsaved edits, Undo/Redo, debouncing, save/close flushes, reopening, project switches, collision handling, failed publication, and safe unlinking. A mock game directory is used; live reload in a running game still needs validation.

Authoring tests cover supported fields, missing shader inputs, active-layer resets, light type conversion, color temperature, exposure and cone parameters, capture immutability, and subset materials. Catalog tests cover persistence, folders, invalid moves, missing assets, and corrupt catalogs. The authoring GPU test exercises inspector edits and focus preservation, DDS material and mesh previews, cache reuse/invalidation, project-switch cancellation, retry, and packaging exclusions. Its artifacts are under `.work/authoring`.

Camera tests cover drone movement and mouse look in Y-up and Z-up scenes, diagonal speed, speed adjustment, cursor release, focus loss, text-entry shortcuts, and switching back to Orbit.

Texture tests cover referenced layers, Windows-style paths, authoring-layer overrides, missing and cleared assets, material sampling settings, and a four-color DDS texture verifying both UV axes across nine tiles (including negative UVs and UVs above one) in the Remix viewport.

Alpha tests cover captured blend state on shared geometry, indexed vertex opacity, material overrides and persistence, and glTF blend ingestion. The GPU test checks cutout holes, smooth and additive blending, override controls, save/reopen, Undo/Redo, and vertex-alpha modulation; its images are under `.work/alpha`.

Project-session tests cover camera round trips, saved-view sidecars, close/cancel and failed-save behavior, command-line startup, recent history, and write failures. GPU tests check Layout and Remix save previews and compact layout at 100%, 150%, and 200% scale, including resize behavior, dialogs, and picking. Session screenshots are retained under `.work/session_ui`. Test runs use isolated recent-project settings.

The pinned upstream Remix test suite has independent failures: two referenced Python test files are absent, and its SDL `offscreen` smoke test cannot find a present-capable queue on the tested driver. Its desktop smoke test runs successfully. Our GPU test exercises the patched runtime through the application.

## Third-party files

The bootstrap copies AperturePBR modules and their license notices from the Remix checkout into ignored `app/materials` for local use. These modules are not included in this repository's authored source. Dependencies and material modules retain their own licenses; see `THIRD_PARTY.md`.

Multiple-capture tests cover legacy workfiles, exclusive scene composition, shared unsaved replacements, layer ordering and offsets, coordinate systems, duplicate and missing captures, undo/redo, save/reopen, capture immutability, packaging, per-capture cameras, pending inspector edits, and renderer reuse.

Placement tests cover standalone spawning, parent selection, pose preservation under transformed parents, reference roots, duplicate attachments, cycle rejection, undo/redo, save/reopen, and capture immutability. Gizmo tests cover both up axes, letterboxed projections, constrained moves, rotation, snapping, cancellation, and one-step commits. The placement GPU test verifies mesh picking over the rendered viewport, spawning without a capture selection, live light and mesh previews, restored illumination after cancellation, and rejection of stale preview requests. Artifacts are under `.work/placement`.

Surface placement tests cover nearest-hit depth, mirrored transforms, self-exclusion, ground fallback, emitter clearance, and both up axes. The light placement GPU test creates each analytical light type on a running scene in metre and centimetre units, verifies visible illumination, and exercises Ctrl+T, Drone placement, inspector focus, and undo. Artifacts are under `.work/light_placement`.

Parent-picker tests cover nearest mesh hits, capture-hash normalization, transformed parents, referenced mesh roots, hover selection, letterboxing, cancellation, deletion, detach, and Undo/Redo. Native tests verify capture-free attachment definitions and game-relative transforms.

Replacement-format tests cover explicit transform operations, capture matrix roundoff, mirrored scales, gimbal-lock rotations, shear and animation preservation, light bounds, metadata, and publication without mutating project or capture files.

When the runtime color patch is applied to `REMIX_SOURCE_DIR`, `remix_usd_light_color` exercises its conversion helper with the real USD and Remix vector types. Tests cover all five analytical light types, RGB and mixed colors through live publication and packaging, legacy light attributes, and rejection of malformed values without changing defaults. `build/remix_light_color_tests /path/to/mod.usda` also verifies a supplied mod's colors without modifying it.

Game-inheritance tests cover first-match hash conflicts, automatic link ordering, retained lights and meshes, material bindings and texture paths, suppression of the original draw call, unrelated mods, old link aliases, repeated publication, deletion, Undo, reopen and safe Unlink.
