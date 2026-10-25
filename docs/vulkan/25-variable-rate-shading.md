# Chapter 25: Variable Rate Shading

> **Objectives:** Explain what variable rate shading saves and where it is safe; enable `VK_KHR_fragment_shading_rate` and read its features and limits; set a static per-draw rate and a dynamic rate at command time; generate a shading-rate mask with a Sobel compute pass and attach it to dynamic rendering; tune compute workgroup size with specialization constants. Chapter 26 reuses a compute-generated mask for its froxel volume, and Chapter 27 carries the jitter-and-reproject pattern into temporal antialiasing.

> **Flint note:** Flint viewports are mostly flat - empty grid background, uniform faces, large even fills - with detail concentrated on silhouette and highlight edges. That distribution is ideal for VRS: shade flats at 2x2, edges at 1x1. Gate it behind an extension check, since not every machine Flint runs on exposes the extension.

## 25.1 Shade less where the eye will not notice

Without VRS every pixel runs the fragment shader once: rate 1x1. A 2x1 or 1x2 rate shades one pair with a single invocation and replicates the result; 2x2 shades four with one. Vertex, rasterization, and depth work is unchanged, so the saving is purely fragment cost - which dominates a heavy lighting pass.

The idea comes from VR foveated rendering: the gaze center runs full rate, the periphery drops. The generalization is that low-frequency regions - skies, distant walls, flat fills - tolerate coarse shading, while edges, specular highlights, and text do not. Rates above 2x2 usually show block artifacts, so this guide stays at 1x1 through 2x2.

This guide derives the rate from image content: a Sobel edge detector over last frame's luminance keeps 1x1 where the gradient is strong and drops to 2x2 where it is flat.

## 25.2 Three places to set the rate

`VK_KHR_fragment_shading_rate` offers three control levels, from coarse to fine:

1. **Per draw.** Bake a rate into the pipeline with `VkPipelineFragmentShadingRateStateCreateInfoKHR`, or change it with `vkCmdSetFragmentShadingRateKHR`.
2. **Per primitive.** Write the `PrimitiveShadingRateKHR` builtin from a vertex or mesh shader.
3. **Per tile via attachment.** Supply a small `R8_UINT` image to the render pass; each texel encodes the rate for a tile of pixels. This is the path this guide implements, because the rate follows the image rather than the draw.

When several levels are active they combine through combiner ops (`KEEP`, `REPLACE`, `MIN`, `MAX`, `MUL`). `MIN` is the conservative choice when combining: it never shades coarser than the strictest request. The examples below use `KEEP` unless combining, which is the actual default state.

> **Misconception: "VRS lowers the render resolution."**
> No. The render target keeps its size, and depth and coverage still resolve per pixel. Only fragment invocations are shared across pixels. That is why edges stay geometrically sharp while shading can band: a 2x2 quad keeps four depth values but one color. If silhouettes look chunky, the problem is elsewhere.

## 25.3 Enabling the extension

VRS is a device extension on recent hardware only, so check before use. Request the extension name at device creation, then query features and properties:

```cpp
VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsrFeat{
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
VkPhysicalDeviceFeatures2 feats{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
feats.pNext = &fsrFeat;
vkGetPhysicalDeviceFeatures2(physDevice, &feats);

if (!fsrFeat.attachmentFragmentShadingRate)
    return; // no attachment VRS on this device: use the plain path

// Enable the bits you need at device creation by chaining fsrFeat
// (pipelineFragmentShadingRate, primitiveFragmentShadingRate,
//  attachmentFragmentShadingRate) into VkDeviceCreateInfo::pNext.
```

Properties tell you the legal tile sizes and combiner support:

```cpp
VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsrProps{
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR };
VkPhysicalDeviceProperties2 props{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
props.pNext = &fsrProps;
vkGetPhysicalDeviceProperties2(physDevice, &props);
// Respect fsrProps.minFragmentShadingRateAttachmentTexelSize
// and maxFragmentShadingRateAttachmentTexelSize when sizing the mask.
```

A static per-draw rate needs no image at all:

```cpp
VkPipelineFragmentShadingRateStateCreateInfoKHR rateState{
    VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR };
rateState.fragmentSize = { 2, 2 };
rateState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
rateState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
// Chain rateState into VkGraphicsPipelineCreateInfo::pNext.
```

Or set it dynamically inside a command buffer with `vkCmdSetFragmentShadingRateKHR`, declared as `VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR`:

```cpp
VkExtent2D size{ 2, 1 };
VkFragmentShadingRateCombinerOpKHR ops[2] = {
    VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR,
    VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR
};
vkCmdSetFragmentShadingRateKHR(cmd, &size, ops);
```

This path needs the `pipelineFragmentShadingRate` feature enabled at device creation and `VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR` listed in `VkPipelineDynamicStateCreateInfo`. Either way, no shader changes are needed. See the [`VK_KHR_fragment_shading_rate`](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_fragment_shading_rate.html) specification.

## 25.4 Computing the rate mask

The mask is an `R8_UINT` image at mask resolution - framebuffer size divided by texel size, rounding up - written by a compute pass over last frame's lit color. This guide dispatches one thread per mask texel with a `{16, 16}` texel size, so dispatch domain, store coordinates, and texel size agree: dispatch `ceil(W/16)` x `ceil(H/16)` threads (120x68 for 1920x1080), and each thread stores to its own mask coordinate. This differs from the specification's minimal `{1, 1}` texel example, which shades per pixel with a full-resolution mask; `{16, 16}` cuts mask memory and compute by 256x at the cost of coarser tile granularity, which is sufficient for flat-versus-edge classification.

Each thread samples the top-left representative pixel of its 16x16 tile and loads that luminance into shared memory, so the 3x3 Sobel neighborhood over representative luminances costs one fetch per thread instead of nine:

```glsl
layout(local_size_x = 16, local_size_y = 16) in;
shared float lum[18][18]; // 16x16 mask texels plus 1-texel halo

uint lx = gl_LocalInvocationID.x + 1;
uint ly = gl_LocalInvocationID.y + 1;
ivec2 maskCoord = ivec2(gl_GlobalInvocationID.xy);
ivec2 srcPixel = maskCoord * 16; // top-left representative pixel of this tile
lum[ly][lx] = luminance(texelFetch(colorTex, srcPixel, 0).rgb);
// Border threads additionally load the halo ring (omitted for brevity).
barrier();
```

After the barrier:

```glsl
float dx = lum[ly-1][lx-1] - lum[ly-1][lx+1]
         + 2.0 * lum[ly][lx-1] - 2.0 * lum[ly][lx+1]
         + lum[ly+1][lx-1] - lum[ly+1][lx+1];
float dy = lum[ly-1][lx-1] + 2.0 * lum[ly-1][lx] + lum[ly-1][lx+1]
         - lum[ly+1][lx-1] - 2.0 * lum[ly+1][lx] - lum[ly+1][lx+1];
float d = dx * dx + dy * dy;
```

The texel encoding follows the specification: for a stored byte `v`, the shaded quad is `2^((v >> 2) & 3)` wide by `2^(v & 3)` high. So `0` means 1x1 and `(1 << 2) | 1` means 2x2:

```glsl
uint rate = (1u << 2u) | 1u; // 2x2 default for flat areas
if (d > 0.1)
    rate = 0u;               // 1x1 on edges
imageStore(rateImg, maskCoord, uvec4(rate, 0, 0, 0));
```

The threshold `0.1` is tuned by eye: lower keeps more of the frame at full rate, higher admits banding on soft gradients. Since the mask derives from the previous frame, dilating the 1x1 regions by one texel hides one frame of lag under camera motion.

## 25.5 Attaching the mask to rendering

Size the mask from the framebuffer divided by the texel size, rounding up. A 1920x1080 target at 16x16 needs a 120x68 mask. Dispatch the compute pass at that same mask resolution so dispatch domain, store coordinates, and `shadingRateAttachmentTexelSize` all agree. Before use, transition it to the dedicated layout with the dedicated stage:

```cpp
// Layout: VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR
// Wait stage: VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
// Access: VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR
```

With dynamic rendering, attach it through the `pNext` chain - no `VkRenderPass` object needed:

```cpp
VkRenderingFragmentShadingRateAttachmentInfoKHR srInfo{
    VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR };
srInfo.imageView = rateImageView;
srInfo.imageLayout =
    VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
srInfo.shadingRateAttachmentTexelSize = { 16, 16 };

VkRenderingInfoKHR renderingInfo{
    VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
renderingInfo.pNext = &srInfo;
// ... fill renderArea, color/depth attachments as usual ...
vkCmdBeginRendering(cmd, &renderingInfo);
```

The shading shaders need no modifications: the hardware replicates invocations according to the mask automatically.

## 25.6 Tuning with specialization constants

The 16x16 workgroup above is not ideal on every GPU. Specialization constants let one shader take different values per pipeline:

```glsl
layout(constant_id = 0) const uint TILE = 16u;
layout(local_size_x_id = 0, local_size_y_id = 0) in;
```

Then supply the value at pipeline creation:

```cpp
VkSpecializationMapEntry entry{};
entry.constantID = 0;
entry.offset = 0;
entry.size = sizeof(uint32_t);

uint32_t tile = (vendorIsArm ? 8u : 16u);
VkSpecializationInfo spec{};
spec.mapEntryCount = 1;
spec.pMapEntries = &entry;
spec.dataSize = sizeof(tile);
spec.pData = &tile;

VkPipelineShaderStageCreateInfo stage{
    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
stage.pSpecializationInfo = &spec;
// ... create compute pipeline; TILE is now fixed to tile ...
```

The same mechanism serves material toggles and subgroup sizes. Flint's engine code specializes `SUBGROUP_SIZE` for subgroup tuning; the `TILE` example here uses the identical `VkSpecializationInfo` path with a different constant ID (see [`VkSpecializationInfo`](https://registry.khronos.org/vulkan/specs/latest/man/html/VkSpecializationInfo.html)). The driver bakes the value at compile time, so there is no runtime branch.

## Recap

- VRS shares one fragment invocation across 1x2, 2x1, or 2x2 pixels; depth and coverage stay per pixel.
- `VK_KHR_fragment_shading_rate` controls the rate per draw, per primitive, or per tile via an `R8_UINT` attachment; combiner ops resolve conflicts.
- Enable the extension only after checking features; size the mask from `min/maxFragmentShadingRateAttachmentTexelSize`.
- A Sobel-on-luminance compute pass with shared-memory tiling produces the mask; byte `0` is 1x1, `(1 << 2) | 1` is 2x2.
- Attach the mask through `VkRenderingFragmentShadingRateAttachmentInfoKHR` with the dedicated layout and pipeline stage; shaders stay unchanged.
- Specialization constants tune workgroup size and toggles per pipeline without editing and recompiling shader source (a new pipeline compile still occurs).

## Exercises

1. **Flat-versus-edge sweep.** Force the mask to 2x2, then sweep the threshold from `0.05` to `0.4`. Capture both frames and timestamp the lighting pass. State where banding first appears and what each threshold saves.
2. **Tile-size tuning.** Specialize the Sobel workgroup to 8x8 and 16x16, keeping the halo logic correct. Time the pass with timestamp queries and explain which size wins and why.
