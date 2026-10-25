# Chapter 14: Fragment Processing and Textures

> **Objectives:** Follow primitives to fragments; set scissor, depth, stencil, MSAA, blending; choose load/store and dependencies; stage textures, write samplers, bind combined descriptors.
>
> **Flint note:** Flint is flat with no 3D depth, so depth, stencil, and MSAA usually stay off. Blending, `CLEAR` versus `LOAD`, and one sampled atlas matter every frame.

Primitives leave geometry processing in framebuffer coordinates. This chapter follows them onward: rasterization, shading, testing, blending, render-pass framing, and the sampled textures supplying color detail.

## 14.1 Rasterization produces fragments

Rasterization converts each primitive into *fragments*, one per pixel (or per sample) it touches, each carrying varyings, position, and depth.

A fragment is a candidate, not yet a pixel. It must survive scissor, run the shader, then pass depth and stencil before blending.

Scissor runs first, before shading. Each `VkRect2D` discards outside fragments:

```c
VkRect2D scissor = {
    .offset = { .x = 0, .y = 0 },
    .extent = { .width = 1280, .height = 720 },
};
vkCmdSetScissor(cmd, 0, 1, &scissor);
```

Scissor is always enabled; covering the render area effectively disables it. Make it dynamic with `VK_DYNAMIC_STATE_SCISSOR` so panels resize it without rebuilding the pipeline. Scissor never moves primitives, only discards, so scissored fragments skip shading and tight scissors cut fill cost.

## 14.2 Fragment shaders color survivors

The fragment shader runs once per survivor. Outputs map to color attachments by location:

```glsl
#version 450 core
layout(location = 0) out vec4 o_color;
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D texGlyph;
void main() {
    float a = texture(texGlyph, v_uv).r;
    if (a < 0.01) discard;
    o_color = vec4(1.0, 1.0, 1.0, a);
}
```

Match output types to attachment formats: float for float/normalized, integer for integer. Up to `maxFragmentOutputAttachments` (at least four) may be written at once.

`discard` kills the fragment: nothing is written. Writing `gl_FragDepth` replaces the interpolated depth and defers the depth test until after shading.

## 14.3 Depth, stencil, and their full state

Depth and stencil decide whether a fragment stays visible given earlier draws. Logically both run *after* the shader, but drivers run them early when side-effect free. `layout(early_fragment_tests) in;` forces that ordering even with image stores.

The full state lives in one struct:

```c
VkPipelineDepthStencilStateCreateInfo ds = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
    .depthCompareOp = VK_COMPARE_OP_LESS,
    .depthBoundsTestEnable = VK_FALSE, .minDepthBounds = 0.0f, .maxDepthBounds = 1.0f,
    .stencilTestEnable = VK_FALSE,
    .front = { .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_KEEP,
               .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_NEVER },
    .back = { .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_KEEP,
              .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_NEVER },
};
```

Depth writes belong to the test, so `depthWriteEnable` needs `depthTestEnable`. Seed unconditionally with test on, writes on, `VK_COMPARE_OP_ALWAYS`.

Depth bias fights z-fighting: `.depthBiasEnable = VK_TRUE` plus constant, clamp, and slope, retuned with `vkCmdSetDepthBias` under `VK_DYNAMIC_STATE_DEPTH_BIAS`.

The depth-bounds test rejects fragments outside `[minDepthBounds, maxDepthBounds]` and needs the `depthBounds` feature; it is dynamic with `VK_DYNAMIC_STATE_DEPTH_BOUNDS` plus `vkCmdSetDepthBounds`.

Stencil masks regions with per-face ops. Each face holds `.failOp`, `.passOp`, `.depthFailOp`, `.compareOp`, `.compareMask`, `.writeMask`, and `.reference`, retuned through `vkCmdSetStencilCompareMask`, `vkCmdSetStencilWriteMask`, and `vkCmdSetStencilReference` with a `VK_STENCIL_FACE_*` mask.

> **Misconception:** "Transparent objects sort themselves."
> They do not. Blending only mixes the current fragment with what is stored. Draw opaque front-to-back, then blended primitives back-to-front.

## 14.4 Multisampling smooths edges, then resolves

MSAA stores several samples per pixel and tests coverage and depth per sample, while shading once per pixel. Sample-rate shading shades per sample at higher cost.

Set image `samples` (for example `VK_SAMPLE_COUNT_4_BIT`, after checking `sampleCounts`) and match it with `rasterizationSamples`. `minSampleShading` sets the uniquely shaded fraction; `pSampleMask` (length `ceil(rasterizationSamples / 32)`) gates coverage; `alphaToCoverageEnable` derives coverage from alpha, `alphaToOneEnable` forces alpha to one afterward.

Sample positions are fixed for 1, 2, 4, 8, and 16 samples, so no query is needed. `VK_EXT_sample_locations` is the carve-out, allowing custom sample positions.

A multisample image is never presented. Resolve it on chip through the subpass:

```c
VkAttachmentReference msaaRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
VkAttachmentReference resolveRef = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
VkSubpassDescription subpass = {
    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
    .colorAttachmentCount = 1, .pColorAttachments = &msaaRef,
    .pResolveAttachments = &resolveRef,
};
```

Set the multisample `storeOp` to `DONT_CARE`; only the resolve target matters. `vkCmdResolveImage` pays a memory round trip and suits partial resolves, taking layouts plus `VkImageResolve` regions (`srcSubresource`, `dstSubresource`, `srcOffset`, `dstOffset`, `extent`).

## 14.5 Blending and logic ops merge results

Blending mixes shader output (source) with stored pixels (destination) in draw order. Standard UI blending:

```c
VkPipelineColorBlendAttachmentState blend = {
    .blendEnable = VK_TRUE,
    .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .colorBlendOp = VK_BLEND_OP_ADD,
    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .alphaBlendOp = VK_BLEND_OP_ADD,
    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                    | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
};
```

Each equation is `result = op(src * srcFactor, dst * dstFactor)`, separately for color and alpha. `blendConstants` (or `vkCmdSetBlendConstants`) feed `CONSTANT_COLOR`; premultiplied atlases use `ONE` as the source color factor. Outputs may be arrays, such as `layout(location = 0) out vec4 o_targets[2]`.

Dual-source blending feeds two outputs into one attachment and needs `dualSrcBlend` (`index = 0` plus `index = 1`, `SRC1_*` factors, capped by `maxFragmentDualSrcAttachments`).

Logic ops are the integer-format counterpart: one shared bitwise op (`logicOpEnable`), applied to integer attachments and ignored for float ones.

## 14.6 Load/store ops and dependencies frame the work

Each attachment declares its memory lifetime at render-pass boundaries:

```c
VkAttachmentDescription color = {
    .format = VK_FORMAT_B8G8R8A8_SRGB, .samples = VK_SAMPLE_COUNT_1_BIT,
    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
};
```

`CLEAR` fills with a clear value, `LOAD` preserves, and `DONT_CARE` leaves tiles undefined and is fastest when every pixel is overwritten. A full repaint with `LOAD` wastes a tile load, while `DONT_CARE` combined with damage tracking that still expects old pixels flickers. `STORE` writes back for presentation; `DONT_CARE` discards transient depth or resolved MSAA data. Pair `UNDEFINED` with `CLEAR` or `DONT_CARE`.

Subpass dependencies order passes and transitions:

```c
VkSubpassDependency dep = {
    .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
    .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
};
```

`VK_SUBPASS_EXTERNAL` covers work outside the pass, such as swapchain acquire. Dependencies carry results forward and transition layouts on chip. This is the minimum for one pass; Chapter 18 builds full multipass graphs, and Chapter 15 explains the barriers behind these dependencies.

## 14.7 Coordinates glue pixels to geometry

A textured vertex carries position plus normalized UV:

```c
struct VertexWithUV {
    float x, y, z, w;
    float u, v;
};
```

UV `(0, 0)` names one corner and `(1, 1)` the opposite; the vertex input adds an attribute at location 1 (two floats, offset 16). Vulkan treats the origin as top-left while many loaders report bottom-left first, so expect a flip until they agree. Flint bakes flipped UVs into its atlas: each glyph stores a sub-rectangle sampled per widget.

The vertex shader forwards UVs (`outUV = inUV`) and the fragment shader samples (`outColor = texture(tex, uv)`): binding 0 holds uniforms, binding 1 holds the texture as `COMBINED_IMAGE_SAMPLER`.

## 14.8 Staging upload, layouts, and the fence wait

An image is typed texels: format, extent, mips, layers, tiling, usage, layout. Three steps make one samplable: `vkCreateImage` with `SAMPLED_BIT` plus `TRANSFER_DST_BIT`, device-local bind, and view plus sampler joined into a descriptor.

Prefer `OPTIMAL` tiling for sampled textures: fast and mip-friendly but host-opaque. `LINEAR` is host-mappable but slow and mip-hostile, for tests only. Optimal images cannot be mapped, so uploads stage through a host-visible buffer.

Allocate and fill the staging buffer, create the device-local image, then record one transient buffer for the copy:

```c
VkImageCreateInfo imgInfo = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
    .extent = { texWidth, texHeight, 1 },
    .mipLevels = mipLevels, .arrayLayers = 1,
    .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
};

VkBufferImageCopy region = {
    .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
    .imageOffset = { 0, 0, 0 }, .imageExtent = { texWidth, texHeight, 1 },
};
```

The layout path is `UNDEFINED`, then `TRANSFER_DST_OPTIMAL`, then `SHADER_READ_ONLY_OPTIMAL`, crossed with barriers around `vkCmdCopyBufferToImage`. For mips, emit one copy per level with extent `max(1, texWidth >> level)` by `max(1, texHeight >> level)`. Keep decoded pixels alive until the copy finishes.

Submit the upload with a fence and wait before freeing staging memory:

```c
vkQueueSubmit(queue, 1, &submitInfo, uploadFence);
vkWaitForFences(device, 1, &uploadFence, VK_TRUE, 10000000000ull); /* 10 s */
```

Record on a family with transfer support; the graphics family qualifies. Chapter 15 explains fences in full.

## 14.9 Samplers, descriptors, and drawing

A sampler decides what `texture(tex, uv)` returns between texels and outside `[0, 1]`:

```c
VkSamplerCreateInfo samplerInfo = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .magFilter = VK_FILTER_LINEAR,
    .minFilter = VK_FILTER_LINEAR,
    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .mipLodBias = 0.0f,
    .anisotropyEnable = VK_FALSE,
    .maxAnisotropy = 1.0f,
    .compareEnable = VK_FALSE,
    .compareOp = VK_COMPARE_OP_NEVER,
    .minLod = 0.0f,
    .maxLod = (float)mipLevels,
    .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    .unnormalizedCoordinates = VK_FALSE,
};
vkCreateSampler(device, &samplerInfo, NULL, &textureSampler);
```

`NEAREST` picks the closest texel; `LINEAR` blends neighbours. `REPEAT` tiles, `MIRRORED_REPEAT` mirrors, `CLAMP_TO_EDGE` stretches the edge, `CLAMP_TO_BORDER` substitutes `borderColor`. Atlases want linear plus clamp-to-edge. Enable anisotropy only with `samplerAnisotropy` and `limits.maxSamplerAnisotropy`.

The shader never sees the `VkImage` directly. Create a 2D view over the image, then package view plus sampler as one descriptor (`dstBinding` must match the GLSL binding):

```c
VkDescriptorImageInfo imgRef = {
    .sampler = textureSampler, .imageView = textureView,
    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
};
VkWriteDescriptorSet texWrite = {
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
    .dstSet = descriptorSet, .dstBinding = 1, .descriptorCount = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .pImageInfo = &imgRef,
};
vkUpdateDescriptorSets(device, 1, &texWrite, 0, NULL);
```

`imageLayout` must equal the actual layout. Then draw as before: bind pipeline, bind the descriptor set, bind the UV vertex buffer, and `vkCmdDraw`. Keep sampler and view alive through the draw.

If the draw renders black, check in order: layout reached `SHADER_READ_ONLY_OPTIMAL`, `dstBinding` matches GLSL, and sampler and view outlive the draw.

## Recap

- Fragments come from rasterization; scissor discards before shading.
- Shaders write `location` outputs; `discard` kills, `gl_FragDepth` defers depth.
- Depth/stencil run early when side-effect free; bias, bounds, and per-face state tune them.
- MSAA shades per pixel, resolves on chip or with `vkCmdResolveImage`.
- Blending mixes in order; dual-source and logic ops cover the rest.
- `loadOp`/`storeOp` set lifetime; `EXTERNAL` guards acquire/present.
- UVs map geometry to texels; staging uploads, samplers filter, one descriptor joins view, sampler, layout.

## Exercises

1. **Blend order in a scissored atlas.** Draw two overlapping translucent atlas quads both ways inside a 256x256 scissor. State which order is correct and why scissor-before-shading cuts fill cost.
2. **Staged upload with explicit sampler.** Upload a 4x4 RGBA buffer through staging, sample it with `LINEAR`/`CLAMP_TO_EDGE` then `NEAREST`/`REPEAT` with UVs in `[0, 2]`. Drop the transfer-destination transition once, record the message, restore it. State which combination an icon atlas needs.

Chapter 15 builds the full model of fences, semaphores, events, and barriers. Chapter 18 builds complete multipass graphs from the load/store and dependency primitives here.
