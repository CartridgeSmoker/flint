# Chapter 23: Clustered Deferred Rendering

> **Objectives:** Contrast forward with deferred costs; build a four-target G-buffer with dynamic rendering and octahedral normals; trace tiles to clusters; assign Activision-style depth bins plus screen tiles on the CPU and consume them on the GPU.

> **Flint note:** Flint viewports carry dozens of inspection lights over dense CAD geometry. The deferred plus clustered path below keeps lighting proportional to visible fragments and nearby lights; its G-buffer targets also feed Flint selection outlines and temporal antialiasing. Chapter 24 adds shadowed lights to this loop; Chapter 26 reuses the G-buffer motion vectors for temporal antialiasing.

## 23.1 Forward shades objects, deferred shades pixels

Forward rendering shades each object with all its lights in one pass. It is flexible about materials, handles opaque and transparent objects identically, supports MSAA, and uses little extra memory. Its cost model is the problem: roughly objects times lights. Every fragment evaluates every light, shaders grow large and register-heavy, and hidden fragments are shaded unless a depth prepass with equal-depth replay removes them first. Set that up with a depth-only prepass followed by the main pass with the depth test set to EQUAL and depth writes disabled; if the main-pass shader writes depth, discards, or otherwise disables Early-Z, the replay shades at full cost and the prepass is pure overhead.

Deferred rendering splits the work. A geometry pass writes surface data into a G-buffer, then a lighting pass evaluates lights once per visible pixel: objects plus lights.

The trade is memory and rigidity. Fullscreen targets are expensive, compressed normals lose precision, every material must fit the same packing, and transparent objects still need a forward pass. Both paths share one problem: without culling, each fragment still tests every light. Tiles and clusters solve exactly that.

## 23.2 From tiles to clusters in five steps

Deering's 1988 triangle processor shaded each pixel once; Saito and Takahashi's G-buffer cached depth and normals per pixel. Shrek on Xbox (2001) shipped the first deferred game renderer, S.T.A.L.K.E.R. popularized it, and CryEngine's 2010 "Reaching the Speed of Light" talk made it standard.

Forward returned in 2012 when AMD's Leo demo used compute shaders to build a per-tile light list and iterate only that subset. That is Forward+. Tiles are dense but need per-tile min/max depth and suffer at depth discontinuities.

Clusters extrude tiles into frustum-shaped 3D cells with per-cell light lists, built from light boxes without a prepass. Persson's Just Cause 3 work and the Chalmers paper made this practical. The objection was memory at high resolution.

Activision's 2017 culling presentation, used in this guide, sidesteps the grid. It keeps XY subdivision (screen tiles) and Z subdivision (depth bins) in two separate compact structures and intersects them at shade time.

## 23.3 A four-target G-buffer without render passes

This guide uses `VK_KHR_dynamic_rendering` (core in Vulkan 1.3) so pipelines need no render-pass objects. The frame graph still owns the textures.

Declare attachment formats at pipeline creation and chain the description onto the pipeline:

```cpp
VkPipelineRenderingCreateInfo pipelineRenderingInfo{
    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
pipelineRenderingInfo.colorAttachmentCount = colorFormatCount;
pipelineRenderingInfo.pColorAttachmentFormats = colorFormats;
pipelineRenderingInfo.depthAttachmentFormat = depthFormat;
pipelineRenderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

pipelineInfo.pNext = &pipelineRenderingInfo;
// Leave pipelineInfo.renderPass as VK_NULL_HANDLE.
```

At draw time, fill one `VkRenderingAttachmentInfo` per target plus depth, then begin:

```cpp
VkRenderingAttachmentInfo colorInfo{
    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
colorInfo.imageView = texture->vk_image_view;
colorInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
colorInfo.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
colorInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

VkRenderingInfo renderingInfo{ VK_STRUCTURE_TYPE_RENDERING_INFO };
renderingInfo.renderArea = { { 0, 0 }, { width, height } };
renderingInfo.layerCount = 1;
renderingInfo.colorAttachmentCount = colorCount;
renderingInfo.pColorAttachments = colorInfos;
renderingInfo.pDepthAttachment = &depthInfo;

vkCmdBeginRendering(cmd, &renderingInfo);
// ... draw opaque geometry ...
vkCmdEndRendering(cmd);
```

The fragment shader writes four targets, with locations matching attachment order:

```glsl
layout(location = 0) out vec4 colorOut;   // albedo
layout(location = 1) out vec2 normalOut;  // octahedral XY
layout(location = 2) out vec4 ormOut;     // occlusion, roughness, metalness
layout(location = 3) out vec4 emissiveOut;

colorOut = texture(globalTextures[albedoIndex], uv);
normalOut = octahedralEncode(normal);
```

Normals compress from three floats to two with octahedral mapping: project onto an octahedron, unfold onto XY, fold the lower hemisphere. Store at half precision; renormalize when lighting:

```glsl
vec2 octahedralEncode(vec3 n) {
    vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    vec2 signP = vec2(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0);
    return n.z < 0.0 ? (1.0 - abs(p.yx)) * signP : p;
}

vec3 octahedralDecode(vec2 f) {
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}
```

Omit alpha for opaque geometry, pack scalars across targets, and consider `R11G11B10F` where range matters more than alpha. Flint TAA also needs a two-component motion-vector target, so that spare packing headroom directly limits extra G-buffer bandwidth.

## 23.4 Clusters as bins intersected with tiles

Sort lights by view depth, slice depth into bins, record each bin's index range, rasterize each footprint into 8x8-pixel tiles as bitfields, then per fragment intersect one bin with one tile.

After sorting, lights overlapping a depth range occupy a contiguous span, so each bin stores one packed `(min, max)` word. Each tile holds one bit per sorted light, set when the light's projected box covers the tile. With 256 lights each tile needs 8 words at `tileY * stride + tileX * wordsPerTile`.

> **Misconception: "Deferred shading makes light count free, and clustering tests every 3D cell."**
> Deferred still evaluates every light per pixel unless lights are culled; it only removed the per-object multiplier. And this clustering never builds a 3D grid: it intersects one depth-range lookup with one 2D-tile lookup at shade time.

## 23.5 Assigning lights on the CPU

Each frame starts on the CPU with sorting. Transform each light center and its radius endpoints into camera space, normalize depth against a tight light far plane (100 m here, for bin precision), and sort only indices so the GPU light array uploads once:

```cpp
struct SortedLight { uint32_t lightIndex; float z, zMin, zMax; };

for (uint32_t i = 0; i < lightCount; ++i) {
    vec4 center = view * vec4(lights[i].worldPosition, 1.0f);
    float r = lights[i].radius;
    float z  = (-center.z - zNear) / (zLightFar - zNear);
    float z0 = (-(center.z + r) - zNear) / (zLightFar - zNear);
    float z1 = (-(center.z - r) - zNear) / (zLightFar - zNear);
    sorted[i] = { i, z, fminf(z0, z1), fmaxf(z0, z1) };
}
qsort(sorted, lightCount, sizeof(SortedLight), compareByZ);
```

Walk the sorted array once to fill bins: a light belongs to every bin its span touches; each bin keeps the smallest and largest sorted position, packed as `(max << 16) | min` with 1-based indices so zero means empty.

Then rasterize tiles in sorted order, so position `i` is the bit index. Skip lights behind the near plane, project the light-sphere AABB to pixels, reject off-screen boxes, clamp to the viewport, and set one bit per covered tile:

```cpp
for (uint32_t i = 0; i < lightCount; ++i) {
    uint32_t li = sorted[i].lightIndex;
    vec4 center = view * vec4(lights[li].worldPosition, 1.0f);
    float r = lights[li].radius;

    // Near-plane clamp: drop fully-behind lights, clamp straddling boxes
    // so the projection stays in front of the camera.
    vec3 bMin = vec3(center.x - r, center.y - r, center.z - r);
    vec3 bMax = vec3(center.x + r, center.y + r, center.z + r);
    if (bMax.z >= -zNear) {
        if (bMin.z >= -zNear) continue; // fully behind near plane
        bMin.z = -zNear; // clamp straddling box
    }

    // Project clamped AABB to pixels -> pixelMin, pixelMax ...

    // AABB rejection: drop fully off-screen boxes before touching tiles.
    if (pixelMax.x < 0 || pixelMax.y < 0
        || pixelMin.x >= int(width) || pixelMin.y >= int(height))
        continue;
    uint32_t firstTileX = uint32_t(max(pixelMin.x, 0)) / TILE_SIZE;
    uint32_t lastTileX = uint32_t(min(pixelMax.x, int(width) - 1)) / TILE_SIZE;
    uint32_t firstTileY = uint32_t(max(pixelMin.y, 0)) / TILE_SIZE;
    uint32_t lastTileY = uint32_t(min(pixelMax.y, int(height) - 1)) / TILE_SIZE;

    for (uint32_t y = firstTileY; y <= lastTileY; ++y)
        for (uint32_t x = firstTileX; x <= lastTileX; ++x)
            tileBits[y * tileStride + x * wordsPerTile + i / 32] |= (1u << (i % 32));
}
```

Upload the light array once, plus the remap, bin, and tile buffers each frame. CPU cost is a sort plus bounded rasterization: lights times covered tiles, not pixels.

## 23.6 Lighting on the GPU

The lighting pass below is a compute shader dispatched over the framebuffer with one thread per pixel (`gl_GlobalInvocationID.xy` is the pixel coordinate): it reverses the process per thread - recompute linear depth, unpack the bin range, locate the tile, scan only that range testing bits:

```glsl
vec4 posView = worldToCamera * vec4(worldPosition, 1.0);
float linearD = (-posView.z - zNear) / (zLightFar - zNear);
uint binIndex = min(uint(linearD / BIN_WIDTH), uint(NUM_BINS) - 1u);
uint binWord = bins[binIndex];
uint minId = binWord & 0xFFFFu;
uint maxId = (binWord >> 16u) & 0xFFFFu;

if (maxId != 0u) {
    minId -= 1u; maxId -= 1u; // stored 1-based; zero means empty

    uvec2 tile = gl_GlobalInvocationID.xy / uvec2(TILE_SIZE);
    uint stride = uint(NUM_WORDS) * (resolution.x / uint(TILE_SIZE));
    uint address = tile.y * stride + tile.x * uint(NUM_WORDS);

    for (uint id = minId; id <= maxId; ++id) {
        uint wordId = id / 32u;
        uint bitId = id % 32u;
        if ((tiles[address + wordId] & (1u << bitId)) != 0u) {
            color += shadePointLight(albedo, orm, normal,
                worldPosition, viewDir, F0, NoV, lights[lightIndices[id]]);
        }
    }
}
```

Threads in empty bins exit after one fetch. Keep the scalar loop as the culling reference; the production shader uses an equivalent subgroup path that reduces register pressure by sharing tile-word loads and light parameters across invocations in the subgroup. The same structures serve forward rendering with interpolated surface data.

## Recap

- Forward costs objects times lights; deferred costs objects plus lights, at the price of G-buffer memory and a forward transparent pass.
- Activision's split intersects depth bins with screen tiles instead of a 3D grid.
- Dynamic rendering declares formats at pipeline creation and begins with attachment arrays.
- The G-buffer stores albedo, octahedral normals, packed material terms, and emissive.
- The CPU sorts light indices, packs per-bin ranges, and rasterizes AABBs into tile bitfields; the GPU intersects one bin range with one tile range per pixel/thread.

## Exercises

1. **Bin occupancy overlay.** Visualize `maxId - minId` per pixel as a heat map with 200 point lights. Stack lights in depth, record the worst-case iteration count, halve `BIN_WIDTH`, and report how maximum and average change.
2. **Tile-culling mismatch hunt.** Skip the near-plane clamp in the CPU tile pass, render the light-bit debug view, and find where behind-camera lights leak onto the screen edge. Restore the clamp, confirm the leak disappears, and explain why the bit test alone cannot catch it.
