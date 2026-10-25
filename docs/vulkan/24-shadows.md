# Chapter 24: Shadows

> **Objectives:** Contrast shadow volumes, shadow mapping, and raytraced shadows; build point-light cubemap shadows with mesh shaders; cull per light in compute and per face in the task shader; emit six indirect draws per light; sample layered cubemaps; size per-light resolution and back shadow maps with sparse residency.

> **Flint note:** Flint viewports carry many small point lights - machine lamps, inspection spots, status LEDs. A full cubemap per light exhausts memory first. GPU culling plus sparse per-light resolution keeps hundreds of shadow-capable lights affordable. Chapter 29 extends the static-light cache below to ray-traced shadows; Chapter 26 consumes these depths alongside the G-buffer for temporal stability.

## 24.1 Three answers to the same question

Shadows sell depth. Every technique answers one question - is this surface visible from the light - with a different tradeoff.

Shadow volumes, proposed by Crow in 1977 and famous from Doom 3, extrude silhouette edges away from the light and count stencil crossings. Edges stay sharp, but every triangle costs geometry work per light and fill rate explodes. The industry moved on.

Shadow mapping, from 1978, is still the default. Render depth from the light, then compare each camera pixel's light-space depth against it. It runs everywhere including mobile and supports filtering (PCF, VSM). Its failure modes have names: acne, Peter Panning, aliasing. Every engine ships its own bias cocktail.

Raytraced shadows trace a ray from each pixel toward each light. They handle thin blockers that maps miss, but cost too much per pixel and need hardware support. This guide uses shadow mapping, accelerated so many lights can cast at once.

## 24.2 The mesh-shader shadow pipeline

Each point light owns six cubemap faces, packed as layers of one cubemap array: light `i` owns layers `6*i` through `6*i+5`. This chapter caps shadow-casting lights at 256 (`MAX_SHADOW_LIGHTS`), so the array holds at most 1536 layers. Four stages:

1. **Coarse cull (compute).** Sphere-test each instance against each light; emit per-light `(instance, meshlet)` lists.
2. **Command build (compute).** Six indirect mesh dispatches per non-empty light, packing light and face into one word.
3. **Fine render (task plus mesh).** Task shader re-culls per meshlet and face; mesh shader transforms survivors and writes `gl_Layer`.
4. **Sample (lighting).** Compare light-space depth against the cubemap.

No fragment shader runs in the shadow pass: only depth matters.

## 24.3 Coarse culling per light

Mesh instances and lights form a 2D grid: thread `gl_GlobalInvocationID.x` maps to `light = id % activeLights` and `instance = id / activeLights`. Transparent meshes are skipped, and each surviving instance inflates its world sphere before the sphere-sphere test. Counters are reset up front by thread 0 followed by a barrier so no cull thread observes a stale count:

```glsl
layout(local_size_x = 32) in;
if (gl_GlobalInvocationID.x == 0)
    for (uint l = 0; l < activeLights; ++l) perLightCount[l] = 0;
barrier();
memoryBarrierBuffer();
```

(In production prefer a `vkCmdFillBuffer` reset or a dedicated one-thread-per-light reset dispatch before the cull dispatch; the thread-0-plus-barrier form above only orders threads inside its own workgroup, so it is sound here only when the reset and the cull are separated by a dispatch boundary or a full-device barrier.)

```glsl
uint lightIdx = gl_GlobalInvocationID.x % activeLights;
if (lightIdx >= activeLights) return;
uint instIdx = gl_GlobalInvocationID.x / activeLights;
if (instIdx >= meshInstanceCount) return;

vec4 sphere = meshBounds[meshDraws[instDraw].index];
vec4 centerW = models[instIdx] * vec4(sphere.xyz, 1.0);
float radius = sphere.w * length(models[instIdx][0]) * 1.1;
if (!sphereIntersects(centerW.xyz, radius, lights[lightIdx].pos,
                       lights[lightIdx].radius)) return;

uint base = atomicAdd(perLightCount[lightIdx], meshDraws[instDraw].meshletCount);
for (uint m = 0; m < meshDraws[instDraw].meshletCount; ++m)
    meshletPairs[lightIdx * maxPairsPerLight + base + m] =
        uvec2(instIdx, meshDraws[instDraw].meshletOffset + m);
```

Sizing and counter discipline: `meshletPairs` holds `MAX_SHADOW_LIGHTS * maxPairsPerLight` entries with `maxPairsPerLight = per_light_max_instances * maxMeshletsPerInstance`, sized for the worst case (every instance visible to one light). Each `perLightCount[light]` counts meshlet pairs, is zeroed before the pass, grows only by `atomicAdd`, and is clamped against `maxPairsPerLight` on append so an over-full light drops further pairs instead of overflowing into the next light's range. `per_light_max_instances` is therefore both the array stride and the overflow bound.

Per-light counters are zeroed by thread zero first (see above). Empty lights skip all later work.

## 24.4 One indirect command per face

A second compute pass turns counts into dispatches. Each non-empty light contributes six faces. The indirect buffer holds only Vulkan indirect commands - `VkDrawMeshTasksIndirectCommandEXT` is three words (`groupCountX, groupCountY, groupCountZ`), with no room for a light/face tag. The packed light/face word therefore rides in a separate sideband array indexed by `gl_DrawID`, not inside the indirect command. Size the indirect buffer as `commandCount * 12` bytes (three `uint`s per record); the `uvec4` below is presentation only, showing the three command words plus the sideband word that lands in the parallel array:

```glsl
uint lightIdx = gl_GlobalInvocationID.x;
if (lightIdx >= activeLights) return;
uint visible = perLightCount[lightIdx];
if (visible == 0) return;
uint slot = atomicAdd(totalCommandCount, 6);
uint groups = (visible + 31) / 32;
for (uint f = 0; f < 6; ++f) {
    // Indirect command: exactly three words per record.
    meshDrawCmds[slot + f] = uvec3(groups, 1, 1);
    // Sideband, parallel to the indirect buffer, read via gl_DrawID.
    lightFaceSideband[slot + f] = (lightIdx << 16) | f;
}
```

(If shown as a single `uvec4(groups, 1, 1, (lightIdx << 16) | f)`, the `.xyz` is the indirect command and the `.w` is the sideband word - do not allocate 16 bytes per indirect command.)

The single `vkCmdDrawMeshTasksIndirectEXT` that follows therefore draws every face of every lit shadow map without CPU intervention.

> **Misconception: "Multiview renders all six faces in one invocation."**
> It can broadcast one vertex to several views, but mesh-shader multiview support is uneven and the EXT path used here does not implement it reliably. Six explicit commands cost six task launches and buy portability. Treat multiview as a future optimization, not a prerequisite.

## 24.5 Task shader: fine and face culling

The task shader runs once per 32-meshlet group. It unpacks light and face from the sideband word indexed by `gl_DrawID` (section 24.4), fetches the pair written above, then applies cone, sphere, and cubemap-face tests in order. The whole chapter uses the EXT dialect: payload via `taskPayloadSharedEXT`, mesh-task launch via `EmitMeshTasksEXT`.

Face culling is the win. `getCubeFaceMask` tests the meshlet AABB against the six 90-degree frustums meeting at the light and returns a 6-bit mask; the current face's bit decides survival. Bit-order convention: bit `f` corresponds to face `f` with `0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z`, matching the cubemap-layer order `layer = 6 * light + face` used in section 24.6:

```glsl
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 32) in;
taskPayloadSharedEXT TaskPayload {
    uint meshletIndices[32];
    uint packedLightFace;
} payload;

void main() {
    uint lane = gl_LocalInvocationID.x;
    uint mi = gl_WorkGroupID.x * 32 + lane;
    uint packed = lightFaceSideband[gl_DrawID];
    uint lightIdx = packed >> 16, face = packed & 0xF;
    uint visible = perLightCount[lightIdx];
    bool accept = (mi < visible);
    uvec2 pair = accept
        ? meshletPairs[lightIdx * maxPairsPerLight + mi]
        : uvec2(0, 0);
    if (accept) {
        // ... fetch model, center, radius, cone axis/cutoff ...
        accept = !coneCull(worldCenter, radius, axis, cutoff, lightPos);
    }
    if (accept)
        accept = sphereIntersects(worldCenter, radius, lightPos, lightRadius);
    if (accept) {
        uint mask = getCubeFaceMask(lightPos, worldCenter - vec3(radius),
                                    worldCenter + vec3(radius));
        accept = ((mask >> face) & 1u) != 0u;
    }
    uvec4 ballot = subgroupBallot(accept);
    if (accept)
        payload.meshletIndices[subgroupBallotExclusiveBitCount(ballot)] = pair.y;
    if (lane == 0)
        payload.packedLightFace = packed;
    EmitMeshTasksEXT(subgroupBallotBitCount(ballot), 1, 1);
}
```

Two guards matter. The trailing-group guard (`mi >= visible` forces `accept = false`) stops the last partially filled group from reading past `perLightCount` pairs. The ballot-plus-`EmitMeshTasksEXT` publish replaces the legacy task-count write: a zero count launches no mesh work for that group.

In practice, for an interior lamp only one or two faces contain any survivors; the face-bit test rejects the rest before mesh expansion (this is the payoff Figure 24.2's caption calls out: face culling removes most task groups before the mesh stage).

- **Debug toggle:** gate the face test behind a `debugDisableFaceCull` uniform (force `mask = 0x3Fu`) so a face-cull audit can compare mesh-shader invocations with and without culling; see Exercise 1.

## 24.6 Mesh shader: layered rendering

The mesh shader is deliberately thin. It resolves `layer = 6 * light + face`, transforms by that face's view-projection, and stamps `gl_Layer`. Counts go through `SetMeshOutputsEXT` and indices through `gl_PrimitiveTriangleIndicesEXT` (EXT dialect throughout; indices go through the per-triangle array, not packed-index helpers):

```glsl
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 32) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;
taskPayloadSharedEXT TaskPayload {
    uint meshletIndices[32];
    uint packedLightFace;
} payload;

void main() {
    uint lightIdx = payload.packedLightFace >> 16;
    uint face = payload.packedLightFace & 0xF;
    int layer = int(6 * lightIdx + face);
    uint mi = payload.meshletIndices[gl_WorkGroupID.x];
    uint vertexCount = meshlets[mi].vertexCount;
    uint triangleCount = meshlets[mi].triangleCount;
    SetMeshOutputsEXT(vertexCount, triangleCount);

    // ... vertex loop: gl_MeshVerticesEXT[i].gl_Position =
    //         viewProj[layer] * (model * vec4(pos, 1.0)); ...

    // Index loop: one uvec3 per triangle via gl_PrimitiveTriangleIndicesEXT.
    for (uint t = gl_LocalInvocationID.x; t < triangleCount; t += 32)
        gl_PrimitiveTriangleIndicesEXT[t] = fetchTriangle(meshlets[mi], t);

    // Layer loop: every primitive needs its own gl_Layer. A single write
    // leaves the remaining 123 layers undefined, so stride all 32 threads
    // over four entries each (32 * 4 = 128 >= 124) to cover every triangle.
    // Strided (not contiguous-chunk) assignment avoids per-primitive bank
    // conflicts on the primitive output.
    for (uint k = 0; k < 4; ++k) {
        uint p = gl_LocalInvocationID.x + k * 32;
        if (p < triangleCount)
            gl_MeshPrimitivesEXT[p].gl_Layer = layer;
    }
}
```

Because each face has its own projection, one vertex cannot cover several faces - hence the duplicated commands. Layered rendering keeps the update in one pass.

## 24.7 Sampling the cubemap

The lighting shader reconstructs the light-to-surface vector, fetches stored depth, and compares with a bias:

```glsl
float vectorToDepth(vec3 v, float farPlane) {
    float major = max(abs(v.x), max(abs(v.y), abs(v.z)));
    float n = 0.01;
    return (farPlane * (major - n)) / ((farPlane - n) * major);
}

vec3 toLight = worldPos - light.worldPos;
float stored = texture(cubemapArray[nonuniformEXT(shadowIndex)],
                       vec4(toLight, float(shadowLayer))).r;
float current = vectorToDepth(toLight, light.radius);
float lit = (current - bias < stored) ? 1.0 : 0.0;
```

This guide leaves shadows hard; add PCF or VSM once the pipeline is stable. Too little bias gives acne, too much detaches shadows (Peter Panning).

## 24.8 Sparse shadow memory

Six full-resolution faces per light is the memory trap: 256 lights at 512 squared depth exceed any budget, yet distant lights cover a handful of pixels. The fix is sparse residency - allocate the full cubemap array address range, back only the tiles each light earns, and reassign pages per frame.

Creation requests both sparse flags, then queries the tile granularity for all bind offsets:

```cpp
VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
info.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT
           | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
vkCreateImage(device, &info, nullptr, &shadowImage);

VkPhysicalDeviceSparseImageFormatInfo2 fmt{
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2 };
fmt.format = shadowFormat; fmt.type = VK_IMAGE_TYPE_2D;
fmt.samples = VK_SAMPLE_COUNT_1_BIT;
fmt.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
fmt.tiling = VK_IMAGE_TILING_OPTIMAL;
vkGetPhysicalDeviceSparseImageFormatProperties2(physDevice, &fmt, &count, props);
uint32_t tileW = props[0].properties.imageGranularity.width;
uint32_t tileH = props[0].properties.imageGranularity.height;
```

A page pool from VMA holds the backing: query `vkGetImageMemoryRequirements` (its `alignment` is the page size), then `vmaAllocateMemoryPages` for `poolSize / (tileW * tileH * formatByteSize)` pages. Dimensionality note: the divisor is the tile byte size, not the tile texel count - it must include the depth-format byte size (for example 4 bytes per texel for `VK_FORMAT_D32_SFLOAT`); omitting it over-allocates by that factor. `alignment` from the requirements is the sparse page size; do not substitute the granularity query for it.

Per-light resolution comes from screen coverage, following the virtual-shadow-map paper. Transform each light AABB to view space, build each tile-plus-slice cluster AABB, test overlap, and compute the solid angle the cluster subtends at the light. Resolution follows from cluster pixel area over solid angle; keep the maximum. With cluster screen area `P` (pixels squared), cluster solid angle `Ω` (steradians), and one-face solid angle `Ω_face = 4 * π / 6`:

```
R = clamp(ceil(sqrt(P * Ω_face / Ω)), 64, 512)
```

A distant lamp drops to 64 squared while a nearby light keeps 512.

Binding records one bind per resident tile and submits on a sparse-capable queue - one whose `VkQueueFamilyProperties.queueFlags` includes `VK_QUEUE_SPARSE_BINDING_BIT` - signaling a semaphore the shadow pass waits on:

```cpp
VkBindSparseInfo bind{ VK_STRUCTURE_TYPE_BIND_SPARSE_INFO };
bind.imageBindCount = static_cast<uint32_t>(layerBinds.size());
bind.pImageBinds = layerBinds.data();
bind.signalSemaphoreCount = 1; bind.pSignalSemaphores = &sparseSemaphore;
vkQueueBindSparse(sparseQueue, 1, &bind, VK_NULL_HANDLE);
```

Unbound tiles read as undefined, so never sample a region with no backing, and rebind before first use whenever resolution changes.

## Recap

- Volumes lose on geometry and fill rate; raytracing is correct but costly; mapping wins on portability.
- Point shadows pack six faces per light as cubemap-array layers (cap: 256 shadow lights, 1536 layers); compute culls per light, then emits six indirect dispatches per non-empty light. The indirect buffer holds three-word `VkDrawMeshTasksIndirectCommandEXT` records; the packed light/face word rides a sideband array indexed by `gl_DrawID`.
- The task shader cone-, sphere-, and face-culls meshlets (face-bit order `0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z`), guards the trailing group with `mi >= visible`, and compacts survivors by ballot into a `taskPayloadSharedEXT` payload launched with `EmitMeshTasksEXT`; the mesh shader sizes outputs with `SetMeshOutputsEXT`, emits indices via `gl_PrimitiveTriangleIndicesEXT`, and writes `gl_Layer = 6 * light + face` with four strided writes per thread covering all 124 primitives.
- Sampling compares encoded light distance against cubemap depth with a tuned bias; filtering comes later.
- Sparse residency backs only earned tiles: query granularity, pool `poolSize / (tileW * tileH * formatByteSize)` pages (`alignment` is the page size), size by `R = clamp(ceil(sqrt(P * Ω_face / Ω)), 64, 512)`, bind with `vkQueueBindSparse` on a `VK_QUEUE_SPARSE_BINDING_BIT` queue.
- Debug toggle: force the face mask to `0x3F` to audit face-cull savings (Exercise 1).
- Static lights need not re-render every frame; the caching policy (dirty tracking, face-granular refresh) is covered with the ray-traced shadow cache in Chapter 29.

## Exercises

1. **Face-cull audit.** Disable `getCubeFaceMask` and record mesh-shader invocations with timestamp queries. Re-enable it on a single-face scene and explain the ratio from the mask bits.
2. **Sparse sizing sweep.** Fix all lights at 512 squared, record committed pages, then enable solid-angle sizing and dolly from far to near. Plot pages versus distance and state where a light drops a tile row.
