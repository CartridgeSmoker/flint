# Chapter 22: GPU-Driven Rendering

> **Objectives:** Split meshes into meshlets with spheres and cones; generate them with meshoptimizer; filter in a task shader; expand in a mesh shader; build a depth pyramid; run compute occlusion culling with indirect draws. Chapters 23 and 24 then build lighting and material techniques on top of the GPU-driven submission introduced here.

> **Flint note:** Flint viewports hold large datasets where per-object CPU culling is the bottleneck: hundreds of thousands of parts, each tested and submitted from the CPU. One indirect mesh-shader draw driven by GPU culling submits the dataset once and keeps hidden work off the rasterizer and fragment stage.

## 22.1 Move selection onto the GPU

CPU frustum culling works while scenes are small: test each AABB, submit survivors. It breaks down at scale. It is serial work at the top of the frame, it answers membership but not occlusion, and it works at mesh granularity, so a terrain chunk draws in full even when only a corner is visible.

The GPU-driven answer submits everything once and discards hidden parts in shaders. Meshes become meshlets of 64 vertices and 124 triangles with a bounding sphere and facing cone. A task shader rejects back-facing and off-screen meshlets, a mesh shader expands survivors, and a compute pass adds occlusion culling against last frame's depth while writing the draw commands itself.

## 22.2 Meshlets: small, cullable chunks

A meshlet is a locality-preserving cluster: vertex indices into the original mesh plus packed local triangle indices. Sixty-four vertices fit the thread group; the hardware budget is 126 triangles and 124 remain usable after reserving the primitive-count word. Each meshlet carries a bounding sphere (one `vec4`) and a normal cone (axis plus cutoff). Spheres beat AABBs: half the bytes, one dot product per plane.

Generation uses meshoptimizer: size working arrays with the bound query, then build:

```cpp
const size_t kMaxVertices = 64;
const size_t kMaxTriangles = 124;

size_t maxMeshlets = meshopt_buildMeshletsBound(
    indexCount, kMaxVertices, kMaxTriangles);

std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
std::vector<unsigned int> meshletVertices(maxMeshlets * kMaxVertices);
std::vector<unsigned char> meshletTriangles(
    maxMeshlets * kMaxTriangles * 3);

size_t meshletCount = meshopt_buildMeshlets(
    meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
    indices, indexCount,
    reinterpret_cast<const float*>(positions), vertexCount,
    sizeof(Position), kMaxVertices, kMaxTriangles, 0.0f);
```

Each `meshopt_Meshlet` is two offsets and two counts into those arrays, not into the original buffers. Local triangle indices fit in one byte because no meshlet holds more than 64 distinct vertices.

Bounds and cones come from a second pass over the result:

```cpp
for (size_t m = 0; m < meshletCount; ++m) {
    const meshopt_Meshlet& ml = meshlets[m];
    meshopt_Bounds b = meshopt_computeMeshletBounds(
        meshletVertices.data() + ml.vertex_offset,
        meshletTriangles.data() + ml.triangle_offset, ml.triangle_count,
        reinterpret_cast<const float*>(positions), vertexCount,
        sizeof(Position));
    // b.center + b.radius is the sphere; b.cone_axis_s8 /
    // b.cone_cutoff_s8 is the quantized cone for upload.
    // b.cone_apex is also returned but left unused here:
    // cone culling below needs only the axis and cutoff.
}
```

Store positions at full precision, quantize normals to 8-bit signed-normalized (`q = int8(round(clamp(n, -1, 1) * 127))`, unpacked as `float(q) / 127.0`) and UVs to half float (`packHalf2x16` on upload, exact `float` on decode), and upload spheres and cones beside them. Keep a per-mesh offset and count so a coarse whole-mesh test can skip its range first.

## 22.3 Task shaders filter

Task and mesh shaders replace the vertex shader. The task shader is a filter: one thread group per 32 meshlets, testing each and forwarding only survivors. The mesh shader runs once per surviving meshlet, reading its slot from the shared payload.

Dispatch is the indirect-count variant so the GPU decides how many draws launch: `vkCmdDrawMeshTasksIndirectCountEXT(cmd, drawBuffer, 0, countBuffer, 0, maxDrawCount, stride)`. The count buffer holds the draw count written by the culling compute pass; the matching barrier uses `VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT` and `VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT` on the consuming side. Inside the task shader, enable the extension and declare the payload once, identically in both stages:

```glsl
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 32) in;
taskPayloadSharedEXT Payload { uint meshletIndices[32]; } payload;
```

Each thread resolves its meshlet (`gl_DrawID` selects the draw, `gl_WorkGroupID.x * 32 + gl_LocalInvocationID.x` selects the meshlet slot within it), unpacks the signed-normalized quantized cone (`float(q) / 127.0` maps the `int8` range back to `[-1, 1]`), and runs cone then frustum tests:

```glsl
bool coneCull(vec3 center, float radius, vec3 axis, float cutoff, vec3 eye) {
    vec3 toCenter = center - eye;
    return dot(toCenter, axis) >= cutoff * length(toCenter) + radius;
}

// per thread:
vec4 centerW = model * vec4(meshlets[mi].center, 1.0);
float radius = meshlets[mi].radius * length(model[0]);
vec3 axis = mat3(model) * (vec3(meshlets[mi].coneAxis.xyz) / 127.0);
float cutoff = float(meshlets[mi].coneCutoff) / 127.0;

bool accept = !coneCull(centerW.xyz, radius, axis, cutoff, cameraPos);
vec4 centerV = worldToCamera * centerW;
for (uint i = 0; i < 6; ++i)
    accept = accept && (dot(frustumPlanes[i], centerV) > -radius);
```

Survivors compact through a subgroup ballot; the group then spawns one mesh workgroup per survivor. The payload holds up to 32 meshlet indices per task group, and mesh workgroup `N` in the spawned grid reads slot `N`:

```glsl
uvec4 ballot = subgroupBallot(accept);
uint slot = subgroupBallotExclusiveBitCount(ballot);
if (accept)
    payload.meshletIndices[slot] = meshletIndex;
barrier();
if (gl_LocalInvocationID.x == 0)
    EmitMeshTasksEXT(subgroupBallotBitCount(ballot), 1, 1);
```

`EmitMeshTasksEXT` is the launch: a zero count spawns no mesh work for that group.

## 22.4 Mesh shaders expand

The mesh shader reads the compacted list like 32 vertex shaders in lockstep. Outputs are arrays: one workgroup emits one meshlet of up to 64 vertices. The payload declaration is byte-identical to the task shader's:

```glsl
layout(local_size_x = 32) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;
taskPayloadSharedEXT Payload { uint meshletIndices[32]; } payload;
layout(location = 0) out vec2 vUv[];
layout(location = 1) flat out uint vDrawIndex[];
```

Each spawned mesh workgroup reads the slot matching its index in the `EmitMeshTasksEXT` grid; `gl_DrawID` recovers the draw so per-draw data (`model`, per-mesh offsets) resolves the same way as in the task shader:

```glsl
uint mi = payload.meshletIndices[gl_WorkGroupID.x];
uint draw = gl_DrawID;
mat4 model = models[meshDraws[draw].modelIndex];
uint vertexCount = meshlets[mi].vertexCount;
uint vertexOffset = meshlets[mi].dataOffset;
uint indexOffset = vertexOffset + vertexCount;
SetMeshOutputsEXT(vertexCount, meshlets[mi].triangleCount);

for (uint i = gl_LocalInvocationID.x; i < vertexCount; i += 32) {
    uint vi = meshletData[vertexOffset + i];
    gl_MeshVerticesEXT[i].gl_Position = viewProj * (model * vec4(vertexPositions[vi], 1.0));
    vUv[i] = vertexUvs[vi];
    vDrawIndex[i] = draw;
}

for (uint i = gl_LocalInvocationID.x; i < meshlets[mi].triangleCount; i += 32) {
    uint base = indexOffset + i * 3;
    gl_PrimitiveTriangleIndicesEXT[i] =
        uvec3(meshletData[base], meshletData[base + 1], meshletData[base + 2]);
}
```

`SetMeshOutputsEXT(vertexCount, primitiveCount)` must precede any output write and runs under uniform control flow. One `uvec3` per triangle replaces the packed 4x8-bit helper, matching the meshoptimizer byte layout after unpacking. The rasterizer consumes `gl_MeshVerticesEXT` directly, so the fragment shader needs no changes.

Portability note: where mesh shaders are unavailable, the same task-shader culling ports to a compute fallback that compacts visible meshlet indices into storage and draws them with a classic indexed indirect draw; the cone, frustum, and pyramid tests are unchanged, only the launch and expansion move.

## 22.5 Depth pyramid generation

Frustum and cone tests cannot see occlusion. The GPU answer samples last frame's depth through a pyramid: each level halves the resolution above it, so any screen-space bounding box maps to roughly one texel.

> **Misconception: "A depth pyramid is just a mipmapped depth buffer."**
> A mipmap pipeline bilinearly averages four texels, inventing depths that exist nowhere in the scene. A depth pyramid reduces with a max (min under reverse-Z) over four `texelFetch` samples stored with `imageStore`, so every texel is a real far-plane sample covering its footprint. Filtering depth is the bug; selecting depth is the technique.

Build it in compute, one dispatch per level with an 8x8 group. Seed level 0 from the depth texture first (blit or sample-copy into pyramid level 0), then reduce level by level so each dispatch reads the previous level as `SHADER_READ_ONLY` and writes the next as `GENERAL`:

```cpp
// Seed: copy scene depth into pyramid level 0, then
// transition level 0 GENERAL -> SHADER_READ_ONLY_OPTIMAL.
copyDepthToPyramidLevel0(cmd, depthImage, pyramid);
uint32_t width = srcWidth / 2, height = srcHeight / 2;
for (uint32_t mip = 1; mip < pyramidMips; ++mip) {
    transitionLevel(pyramid, mip, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    transitionLevel(pyramid, mip, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    width = std::max(1u, width / 2);
    height = std::max(1u, height / 2);
}
```

The shader itself is four fetches and a max:

```glsl
ivec2 base = ivec2(gl_GlobalInvocationID.xy) * 2;
float d0 = max(texelFetch(srcPyramid, base, 0).r,
               texelFetch(srcPyramid, base + ivec2(1, 1), 0).r);
float d1 = max(texelFetch(srcPyramid, base + ivec2(0, 1), 0).r,
               texelFetch(srcPyramid, base + ivec2(1, 0), 0).r);
imageStore(dstPyramid, ivec2(gl_GlobalInvocationID.xy), vec4(max(d0, d1), 0, 0, 0));
```

Use `min` instead of `max` with inverted depth. The coarser level then conservatively represents the farthest surface in its footprint, which is what makes a single lookup sound.

## 22.6 Occlusion culling in compute

The culling compute pass runs before the mesh draw, testing whole meshes (then meshlets the same way). View space follows the `-Z` convention: the camera looks down `-Z`, forward depth is `-z`, and the projection and closest-depth math below assumes view-space centres in front of the camera have negative `z`. Project each bounding sphere to a screen-space box: reject spheres behind the near plane, solve extents on the xz- and yz-planes, scale by the projection diagonal, remap clip `[-1, 1]` to UV `[0, 1]`:

```glsl
bool projectSphere(vec3 c, float r, float zNear,
    float p00, float p11, out vec4 box);
```

Pick the level matching the box size, sample once at its centre, and compare against the sphere's closest depth:

```glsl
vec2 sizePx = vec2(box.z - box.x, box.w - box.y) * vec2(pyramidSize);
float level = floor(log2(max(sizePx.x, sizePx.y)));
float occluder = textureLod(pyramid, (box.xy + box.zw) * 0.5, level).r;
float closest = zNear / (viewCenter.z - radius);
bool visible = (closest <= occluder);
```

Survivors append draw commands for the indirect-count mesh dispatch. The write-to-draw barrier sources the compute write and targets `VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT` (draw dispatch) and `VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT` (payload and meshlet reads):

```cpp
draws[outIndex].drawId = meshInstanceIndex;
draws[outIndex].taskCount = (meshletCount + 31) / 32;
draws[outIndex].firstTask = meshletOffset / 32;
```

The single-sample test is conservative: a false negative here means a visible sphere reported as occluded (an incorrect cull that would pop geometry), while a false positive means a hidden sphere reported as visible (safe over-draw). Sampling the farthest depth in the footprint errs toward false positives, never false negatives, apart from last-frame motion which phase two repairs. Phase one culls against the old pyramid and draws survivors, refreshing depth and pyramid. Phase two retests the rejected set against the fresh pyramid and draws late survivors.

## Recap

- Meshlets group 64 vertices and 124 triangles (126 hardware budget minus the count word) with a sphere and cone; meshoptimizer builds clusters, bounds, and quantized cones (`cone_apex` unused).
- Task shaders test 32 meshlets per group (cone plus frustum) and compact survivors via ballot into `payload`, launching with `EmitMeshTasksEXT`.
- Mesh shaders expand survivors into vertices and `gl_PrimitiveTriangleIndicesEXT` via `SetMeshOutputsEXT` for the rasterizer.
- The depth pyramid seeds level 0 from depth, then max-reduces (min under reverse-Z) with `texelFetch`/`imageStore` (`GENERAL` to `SHADER_READ_ONLY` per level), never bilinear filtering.
- Compute occlusion projects each sphere (`-Z` view space) to a UV box, samples one level, compares closest depth (false negative means culling a visible sphere), and emits indirect-count commands in two phases that repair last-frame misses.

## Exercises

1. **Cone-culling visualizer.** Keep only the cone test and tint meshlets by `accept`. Orbit and confirm back-facing clusters reach task count zero while silhouettes survive. Re-enable the frustum test and record survivors at three angles.
2. **Pyramid level check.** Show pyramid levels in a debug overlay, hand-compute the level for one sphere from its UV-box extent, and verify the shader agrees. Switch to reverse-Z with a `min` reduction and show the over-occluded object reappears.
