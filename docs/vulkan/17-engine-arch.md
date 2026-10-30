# Chapter 17: Engine Architecture

> **Objectives:** Describe the three layers of a small Vulkan-first engine and the direction dependencies may flow; build and run a minimal sample viewer on Windows and Linux; trace a glTF asset from JSON to GPU buffers and draw calls; explain the conductor versus dielectric split in the metallic-roughness PBR model; capture a frame in RenderDoc and map it back to your own submissions. Chapters 19 and 20 then scale the loop introduced here with threaded recording and frame-graph grouping; Chapters 23 (Clustered Deferred Rendering) and 24 (Shadows) build lighting on top.

Most Vulkan introductions stop once a triangle appears. The problem with jumping straight to advanced rendering is that every example drags along setup and boilerplate that hides the technique being taught.

The approach used through the rest of this guide is a small Vulkan-first framework kept deliberately thin so each chapter can focus on one idea. This chapter tours it as an engine-architecture example: three layers, one device owner, one asset path, one debugging workflow.

> **Scope note:** This is the architecture tour. It assumes you have created a `VkDevice`, recorded command buffers, and written descriptor set layouts before. It is not a Vulkan tutorial. It is a map of one small codebase you will extend through the rest of this guide.

## 17.1 The three layers

The framework enforces one rule: a layer may use its own code and code from layers below, never from above. When information must flow upward, the upper layer owns the glue.

There are three layers:

- **Foundation** (`source/foundation`). Allocators, arrays, hash maps, file I/O, logging, strings, time, process launching. Shared by all chapters and intentionally stable.
- **Graphics** (`source/chapterN/graphics`). All Vulkan code. Snapshotted per chapter so a later chapter can refactor freely without breaking an earlier one.
- **Application** (per-chapter `main.cpp` plus shared windowing). Window creation, input, camera control, Dear ImGui backend.

A concrete example keeps the rule honest. Camera math lives in foundation, but mouse handling lives in application. Foundation never includes input headers. Instead the application-layer `GameCamera` reads input and drives the foundation camera.

Foundation trades standard-library comfort for explicit control. Every container takes an allocator at initialization: a TLSF-based heap plus linear, stack, and malloc-backed allocators, a lean vector analogue, a flat hash map for name-to-resource lookup, file helpers, validation-fed logging, fixed-capacity strings, and delta-time helpers.

The heart of the graphics layer is `GpuDevice`: device and swapchain ownership, command buffer discipline, synchronization, and timestamp queries. It is the only path for creating GPU resources:

```cpp
struct GpuDevice {
    BufferHandle  create_buffer(const BufferCreation& bc);
    TextureHandle create_texture(const TextureCreation& tc);
    void destroy_buffer(BufferHandle& handle);
    void destroy_texture(TextureHandle& handle);
};
```

Two conventions run through it. Creation always goes through a `*Creation` struct that collects parameters in one place, so adding a field never changes a call signature. Live resources are referenced by lightweight copyable handles such as `BufferHandle` and `TextureHandle`, never by raw `VkBuffer` or `VkImage` at the application level. The internal `Buffer` record still mirrors Vulkan (`VkBuffer`, allocation, size, usage) but adds a debug name and a parent handle for sub-allocations, which makes frame captures far easier to read. When a handle is destroyed, pass the handle variable itself so the implementation can null it; a stale copy must never alias a recycled slot.

> **Flint note:** The handle-plus-creation-struct split suits Flint's backend. Handles decouple the interface from Vulkan lifetimes, so higher layers never store a `VkBuffer` directly; creation structs give one extension point per resource type when a new flag or debug label is needed.

## 17.2 Building the sample viewer

The sample application on top of this framework is a minimal glTF viewer. Startup loads one file, uploads buffers, textures, and samplers once, builds one descriptor set per mesh, then loops over the records each frame.

You need a GPU with Vulkan 1.1 or newer and a recent LunarG SDK. **Windows:** install the SDK, confirm `vulkaninfo.exe` reports your GPU, open the provided solution, build, and run the helmet sample project. **Linux:**

```bash
source ~/Downloads/1.2.198.1/setup-env.sh
vulkaninfo   # must list your GPU before continuing
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target sample-viewer -- -j "$(nproc)"
./build/sample-viewer/sample-viewer path/to/FlightHelmet.gltf
```

Use `Release` for performance comparisons and `Debug` while reading validation output. If `vulkaninfo` fails, fix drivers and SDK paths before touching framework code. When the window opens on the helmet model with validation silent, the baseline works; stop tuning the build and start reading the loop.

## 17.3 The glTF scene format

glTF was chosen because it is open, JSON-readable, and describes PBR materials natively. Think of the `.gltf` file as an index and the accompanying `.bin` file as bulk data. Resolution takes five hops:

1. **Scenes and nodes:** `scenes[].nodes` lists root nodes; each node optionally references a mesh plus a transform.
2. **Meshes and primitives:** each primitive maps attribute names (`POSITION`, `NORMAL`, `TANGENT`, `TEXCOORD_0`) to accessor indices, plus an `indices` accessor and a `material` index.
3. **Buffers and views:** `buffers[]` names the blobs; `bufferViews[]` carves each blob into byte ranges.
4. **Accessors:** each accessor types one view: component type (such as `5126` for float), element type (`VEC3`), count, and offset.
5. **Materials:** a material points at textures; each texture pairs an image URI with a sampler. The sample helmet material carries base color, a packed metallic-roughness-occlusion map, and a normal map.

Loading follows a fixed order: parse JSON, read each `.bin` blob into CPU memory, create one GPU buffer per buffer view, create textures and samplers, then pack the resulting handles per mesh into a `MeshDraw` record replayed each frame:

```cpp
for (u32 i = 0; i < mesh_draws.size; ++i) {
    MeshDraw& d = mesh_draws[i];
    cmd->bind_vertex_buffer(sort_key++, d.position_buffer, 0, d.position_offset);
    cmd->bind_vertex_buffer(sort_key++, d.tangent_buffer, 1, d.tangent_offset);
    cmd->bind_vertex_buffer(sort_key++, d.normal_buffer, 2, d.normal_offset);
    cmd->bind_vertex_buffer(sort_key++, d.texcoord_buffer, 3, d.texcoord_offset);
    cmd->bind_index_buffer(sort_key++, d.index_buffer, d.index_offset);
    cmd->bind_descriptor_set(sort_key++, &d.descriptor_set, 1, nullptr, 0);
    cmd->draw_indexed(sort_key++, TopologyType::Triangle, d.count, 1, 0, 0, 0);
}
```

Each `d.position_buffer` or `d.index_buffer` above is a `BufferHandle`, and `d.descriptor_set` is the per-mesh set built at load time; the loop never touches a raw Vulkan object. One stream per attribute mirrors glTF accessors directly and keeps each binding legible in a capture. The monotonically increasing `sort_key` is an incrementing ID that keeps each binding legible in a capture, not a submission order; the capture reads top to bottom like the loop. Interleaving is a fine later optimization.

> **Misconception:** "glTF is just a mesh format, so the loader gives me render-ready data."
>
> It does not. glTF gives typed byte ranges plus material metadata. The framework still decides memory placement, image layouts, sampler creation, descriptor layout, and draw packing. When a model renders black, check accessor decoding and descriptor bindings before blaming the asset.

## 17.4 PBR essentials

This guide follows the glTF 2.0 metallic-roughness model with a Trowbridge-Reitz (GGX) specular lobe. Three terms combine per pixel: a **distribution** term for microfacet alignment, a **visibility** term for self-shadowing, and a **Fresnel** term for rising reflectivity at grazing angles via the Schlick approximation.

The key split is conductor versus dielectric. Metals tint reflections with base color and have no diffuse term. Non-metals split energy between a monochrome specular term rooted at 0.04 and a diffuse term proportional to base color. The shader mixes both paths by `metalness`:

```glsl
vec3 conductor = specular_brdf
    * (base_color + (1.0 - base_color) * pow(1.0 - abs(HdotV), 5.0));
float f0 = 0.04;
float fr = f0 + (1.0 - f0) * pow(1.0 - abs(HdotV), 5.0);
vec3 dielectric = mix(diffuse_brdf, vec3(specular_brdf), fr);
vec3 shaded = mix(dielectric, conductor, metalness);
```

Occlusion affects indirect light only and is parked until image-based lighting arrives. Treat this shader as a baseline: force `metalness` to 0 and 1, clamp roughness, and compare captures.

## 17.5 GPU debugging with RenderDoc

CPU debugging shows your calls; GPU debugging shows what the driver consumed. RenderDoc captures a frame as an event list with full resource inspection.

Point RenderDoc at the viewer binary, launch, press F12 on the frame of interest, close the application, and read the capture. Events group by render pass with a stable ID per call. Select a draw to see bound textures, buffer contents, and pipeline state.

Capture the unmodified baseline frame now. Every later chapter asks what changed, and a clean baseline makes regressions obvious.

## Recap

- The engine has three layers with downward-only dependencies: stable foundation utilities, per-chapter graphics behind `GpuDevice`, and a thin application shell with windowing, input, and ImGui.
- `GpuDevice` owns resource creation through `*Creation` structs; live resources travel as `BufferHandle` and `TextureHandle` values, with destroy calls nulling the passed handle.
- The viewer loads one glTF asset at startup, uploads buffers and textures once, then binds per-`MeshDraw` handles and draws each frame with an incrementing `sort_key` per binding for capture legibility.
- Builds need a Vulkan-capable GPU, a current SDK, and passing `vulkaninfo`; CMake drives Linux builds per chapter target.
- glTF resolves as scenes to nodes to meshes to accessors to views to bytes, with materials layering textures over images plus samplers.
- The PBR shader combines GGX distribution, visibility, and Schlick Fresnel, mixing conductor and dielectric paths by `metalness`.
- RenderDoc turns GPU state into an inspectable event list. Capture the baseline before changing anything.

## Exercises

1. **Capture the baseline.** Run the viewer, capture one frame in RenderDoc, and locate the helmet draws by event ID. For one draw, list the bound vertex buffers, the texture-plus-sampler pairs, and the pipeline. Change one scalar such as roughness in the ImGui panel, capture again, and state which events changed.
2. **Load a foreign asset.** Run any other Khronos glTF sample model through the unmodified loader and note what breaks. Is the failure in accessor decoding, a missing channel, a double-sided flag, or an unsupported extension? Record the exact hop in the scene-to-draw chain where your trace diverges.
