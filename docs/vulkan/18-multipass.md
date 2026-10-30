# Chapter 18: Multipass Rendering

> **Objectives:** Pack several rendering phases into one render pass as ordered subpasses; read the current pixel from an earlier subpass with input attachments; order subpasses with explicit `VkSubpassDependency` stage and access masks; use load/store operations and transient images to stay on tile memory; sketch a depth-prepass plus G-buffer deferred renderer in three subpasses. Chapter 23 evaluates the lighting that runs inside the final subpass, and Chapter 20 automates multi-pass ordering with a frame graph (this chapter's single-pass subpass form vs Chapter 20's multi-pass graph).

This chapter ties together pipelines, framebuffers, barriers, and layouts from earlier chapters of this guide. So far every render pass had one subpass: begin, draw, end. Many frames are naturally phased: depth first, then material properties, then lighting. Or scene first, then interface on top. Each phase reads, at the same pixel, what the last one wrote.

Separate render passes can express that, with a barrier, layout transition, and memory round trip between each phase. A single `VkRenderPass` with several subpasses says it all up front, so the driver can keep intermediates on chip. When phases share no pixel, separate render passes are clearer; subpasses pay off when consumer reads producer's texel at the same coordinate.

## 18.1 One render pass, several subpasses

A render pass owns its attachments and an ordered array of `VkSubpassDescription` entries. Each subpass names the attachments it writes as color or depth-stencil, the ones it reads as inputs, and the ones it preserves for a later subpass:

```c
VkAttachmentReference depthRef = { 0 };
depthRef.attachment = 1;
depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

VkAttachmentReference gbufferRef = { 0 };
gbufferRef.attachment = 2;
gbufferRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

VkSubpassDescription subpasses[3] = { 0 };
subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
subpasses[0].pDepthStencilAttachment = &depthRef;

subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
subpasses[1].colorAttachmentCount = 1;
subpasses[1].pColorAttachments = &gbufferRef;
subpasses[1].pDepthStencilAttachment = &depthRef;
```

Subpasses run in array order. Recording moves forward one step at a time:
```c
vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
/* subpass 0: depth prepass */
vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
/* subpass 1: G-buffer fill */
vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
/* subpass 2: lighting */
vkCmdEndRenderPass(cmd);
```

Begin starts subpass 0, and each `vkCmdNextSubpass` advances exactly once within the same command buffer. Create one pipeline per subpass role, since compatibility depends on the subpass attachment counts and formats.

## 18.2 Input attachments: reading the current pixel

An input attachment reads only the current fragment coordinate. A lighting pass loads the texel under `gl_FragCoord.xy` from the tile already being rasterized. On a tiling GPU that read comes from on-chip tile memory instead of DRAM.

```c
VkAttachmentReference lightingInputs[2] = { 0 };
lightingInputs[0].attachment = 2; /* G-buffer color */
lightingInputs[0].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
lightingInputs[1].attachment = 1; /* depth */
lightingInputs[1].layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

subpasses[2].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
subpasses[2].inputAttachmentCount = 2;
subpasses[2].pInputAttachments = lightingInputs;
subpasses[2].colorAttachmentCount = 1;
subpasses[2].pColorAttachments = &swapchainRef;
```

Read the value with `subpassLoad`, not `texture`:

```glsl
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput gbufferIn;
layout(input_attachment_index = 1, set = 0, binding = 1) uniform subpassInput depthIn;

void main()
{
    vec4 material = subpassLoad(gbufferIn);
    float depth = subpassLoad(depthIn).x;
}
```

Here `input_attachment_index` indexes `pInputAttachments`, while `set` and `binding` locate the descriptor, which must use type `VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT`. If a value must survive an intervening subpass that neither reads nor writes it, list it in that subpass `pPreserveAttachments` so the driver carries it through.

> **Misconception:** "An input attachment is just a cheaper sampled texture."
>
> It is a narrower contract. A sampled image reads any texel with filtering, at the cost of a full memory round trip plus explicit barriers and transitions. An input attachment reads only the current fragment, inside one render pass, but the driver can fold the barrier into the subpass dependency and keep the bytes in tile memory. Use input attachments when producer and consumer align per pixel; use sampled textures when they do not.

## 18.3 Dependencies say what flows where

Order alone does not synchronize. Each `VkSubpassDependency` adds the execution and memory dependency between producer and consumer stages, scoped to the render pass so the driver can merge it with tile loads and stores:

```c
VkSubpassDependency deps[2] = { 0 };
deps[0].srcSubpass = 0;
deps[0].dstSubpass = 1;
deps[0].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

deps[1].srcSubpass = 1;
deps[1].dstSubpass = 2;
deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
deps[1].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
```

Name real stages: depth writes leave `VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT`, color writes leave `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT`, input reads enter `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`. Keep access masks narrow, pairing `VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT` with the matching read bit and `VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT` with `VK_ACCESS_INPUT_ATTACHMENT_READ_BIT`. Set `VK_DEPENDENCY_BY_REGION_BIT` when the dependency is per pixel, so tiles flow through independently instead of draining the whole frame. Add only the edges the graph needs: lighting already depends on the G-buffer pass, which depends on depth, so no direct depth-to-lighting edge is required. Use `VK_SUBPASS_EXTERNAL` as source or destination for the two implicit edges into subpass 0 and out of the last subpass, for example to acquire and present the swapchain image.

## 18.4 Store less, keep it on tile

Each attachment declares entry and exit behavior. `loadOp` governs entry: `VK_ATTACHMENT_LOAD_OP_CLEAR` clears at pass start, `VK_ATTACHMENT_LOAD_OP_LOAD` keeps prior contents, `VK_ATTACHMENT_LOAD_OP_DONT_CARE` skips the load because the first using subpass overwrites every pixel. `storeOp` governs exit: `VK_ATTACHMENT_STORE_OP_STORE` writes back to memory, `VK_ATTACHMENT_STORE_OP_DONT_CARE` lets the driver drop the contents when the pass ends. Intermediates want clear-once, drop-at-end:

```c
attachments[0].format = VK_FORMAT_B8G8R8A8_SRGB; /* swapchain */
attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

attachments[1].format = VK_FORMAT_D32_SFLOAT; /* depth, discarded */
attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
attachments[1].finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

attachments[2].format = VK_FORMAT_R16G16B16A16_SFLOAT; /* G-buffer, discarded */
attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; /* subpass 1 overwrites every pixel */
attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
attachments[2].finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
```

Note the explicit `.samples = VK_SAMPLE_COUNT_1_BIT` on every attachment: the field has no safe default, and leaving it zero is a validation error even when multisampling is not in play. Keep `renderArea` covering the framebuffer; when a smaller rect is needed, align it to the tile granularity from `vkGetRenderAreaGranularity`, since pixels outside it are undefined.

A desktop GPU still allocates backing memory but skips pointless loads and stores. A tiling GPU can do better: an attachment used only as color, depth, or input inside the pass may never leave the chip. Mark such images transient:

```c
imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
    | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
```

A transient depth buffer uses `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT` in place of `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, combined with `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT` and `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` when a later subpass reads it as an input attachment. Allocate these images from a memory type with `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT`; on tile architectures the allocation may consume no physical memory, elsewhere it falls back to real memory. Without subpasses, depth and G-buffer contents each round-trip through DRAM; with per-pixel dependencies and `DONT_CARE` stores, only final shaded pixels cross the bus.

For a scoped clear in the middle of a pass, `vkCmdClearAttachments` takes arrays of `VkClearAttachment` and `VkClearRect` and clears only the given rects and layers of the current subpass attachments. Each `VkClearAttachment` clears either one color attachment or the depth-stencil aspect, never both in the same struct. Prefer `VK_ATTACHMENT_LOAD_OP_CLEAR` when the whole attachment needs clearing at pass start so the clear folds into the tile load, reserve `vkCmdClearAttachments` for rect- or layer-scoped touch-ups, and use `VK_ATTACHMENT_LOAD_OP_DONT_CARE` when the first using subpass overwrites every pixel.

## 18.5 Sketch: three-subpass deferred renderer

Deferred shading fits this shape because its phases align per pixel. Subpass 0 renders depth only, with color writes disabled. Subpass 1 renders each material once, testing against that depth and writing albedo, normal, and roughness with no lighting. Subpass 2 draws one fullscreen triangle, loads G-buffer and depth as inputs, reconstructs position, evaluates lights, and writes the swapchain image. The fullscreen triangle needs no vertex buffers; derived positions cover the viewport, fragment runs once per covered pixel.

```c
VkRenderPassCreateInfo passInfo = { 0 };
passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
passInfo.attachmentCount = 3;
passInfo.pAttachments = attachments;
passInfo.subpassCount = 3;
passInfo.pSubpasses = subpasses;
passInfo.dependencyCount = 2;
passInfo.pDependencies = deps;
vkCreateRenderPass(device, &passInfo, NULL, &renderPass);
```

One pixel holds one material, so translucent surfaces need a side buffer rendered with the same depth for rejection, plus a fourth subpass that reads the opaque and translucent buffers as inputs and composites them. Chapter 23 evaluates the lighting that runs inside that final subpass; Chapter 20 automates the same ordering as a multi-pass graph, against this chapter's single-pass subpass form.

> **Flint note:** In this chapter's shape, viewport scene first, interface overlay second, composited in one render pass. The scene subpass writes color and depth; the UI subpass reads the scene color as an input attachment and blends widgets over it, so the compositor receives one finished image with no intermediate round trip. The canonical Flint loop is Chapter 15's two primaries, viewport then UI overlay, submitted in order; Chapter 20 shows the same ordering as a frame graph.

## Recap

- One `VkRenderPass` holds attachments plus ordered subpasses. Advance with `vkCmdNextSubpass`; bind a matching pipeline per subpass.
- Input attachments read only the current pixel via `subpassLoad`, trading generality for tile-local reads through `VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT`.
- Dependencies carry stage masks, access masks, and `VK_DEPENDENCY_BY_REGION_BIT` locality. Name real stages and add only needed edges, with `VK_SUBPASS_EXTERNAL` covering entry and exit.
- `VK_ATTACHMENT_STORE_OP_DONT_CARE` operations and transient plus lazily allocated images keep intermediates on tile; every attachment still needs an explicit `.samples` value.
- `vkCmdClearAttachments` handles rect-scoped mid-pass clears, one color or depth aspect per struct; whole-attachment clears belong in `loadOp`.
- Deferred rendering maps cleanly: depth prepass, G-buffer fill, fullscreen lighting. Translucents need a side buffer and a composite subpass.

## Exercises

1. **Fold two passes into one.** Take a scene-then-post program with an explicit barrier and layout transition between passes. Rewrite it as one render pass with two subpasses joined by a `VK_DEPENDENCY_BY_REGION_BIT` dependency, the intermediate as an input attachment with `VK_ATTACHMENT_STORE_OP_DONT_CARE`. Compare validation output and, on tiling hardware if available, store bandwidth before and after.
2. **Break the dependency on purpose.** In the deferred sketch, drop `VK_DEPENDENCY_BY_REGION_BIT` from the G-buffer to lighting edge, then widen both stage masks to `VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT`. Confirm the image still renders, then explain what parallelism the driver lost and which tile-memory optimization the wide masks may defeat.
