# Chapter 12: Drawing

> **Objectives:**
> - Open a pass instance with correct clears.
> - Bind in order, then draw non-indexed and indexed.
> - Instance and issue indirect draws.
> - Run secondaries.
> - Drive acquire, submit, and present.
> - Rebuild on resize.

Preparation records one buffer per swapchain image; rendering reuses them every frame. This chapter tells that story end to end. Chapter 13 follows vertices downstream through geometry processing, and Chapter 15 carries finished images through synchronization; presentation surfaces are Chapter 7.

> **Flint note:** Flint batches UI as indexed 2D triangles: one pass per frame, one pipeline bound once, then a run of indexed draws from per-image buffers reused across frames, plus one fence per in-flight frame. Instancing and indirect draws stay in reserve.

## 12.1 A render pass instance frames every draw

Every draw runs inside a render pass instance, which binds a pass to a framebuffer for one execution:

```c
void vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo *pRenderPassBegin,
    VkSubpassContents contents);
void vkCmdEndRenderPass(VkCommandBuffer commandBuffer);
```

Use `VK_SUBPASS_CONTENTS_INLINE` for direct draws and `VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS` when secondaries are executed (section 12.6). Each buffer wraps its work the same way:

```c
VkClearValue clearValues[2];
clearValues[0].color.float32[0] = 0.0f;
clearValues[0].color.float32[1] = 0.0f;
clearValues[0].color.float32[2] = 0.0f;
clearValues[0].color.float32[3] = 1.0f;
clearValues[1].depthStencil.depth = 1.0f;
clearValues[1].depthStencil.stencil = 0;

VkRenderPassBeginInfo beginInfo = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
    .pNext = NULL,
    .renderPass = renderer->renderPass,
    .framebuffer = renderer->framebuffers[imageIndex],
    .renderArea = { .offset = { 0, 0 },
        .extent = { renderer->width, renderer->height } },
    .clearValueCount = 2,
    .pClearValues = clearValues,
};

vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
/* pipeline, vertex data, viewport, scissor, draw go here */
vkCmdEndRenderPass(cmd);
```

Clear values are a union of color or depth-stencil:

```c
typedef union VkClearValue {
    VkClearColorValue        color;
    VkClearDepthStencilValue depthStencil;
} VkClearValue;
```

Use `float32[4]` for float formats, `int32[4]` or `uint32[4]` for integer formats, `depthStencil` for depth/stencil. Entry `i` clears attachment `i`; size the array to the highest-indexed attachment with `VK_ATTACHMENT_LOAD_OP_CLEAR`. Only `STORE` attachments survive the pass.

Keep `renderArea` equal to the framebuffer extent; pixels outside it are undefined. Check `vkGetRenderAreaGranularity` before using a smaller rectangle.

## 12.2 Bind in order, then draw

Command-buffer state is sticky, so record in order: begin the pass, bind pipeline, bind descriptor sets, bind vertex/index buffers, set dynamic viewport and scissor, then draw. Set `vkCmdSet*` state after the last pipeline bind, since rebinding can reset it:

```c
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline);
```

Unless pipeline, framebuffer, or geometry changes, buffers are reused; the loop submits `buffers[imageIndex]`.

Viewport and scissor are dynamic here, so these calls supply the values (`NULL` static pointers at pipeline creation). Without the matching `VK_DYNAMIC_STATE_*` entries they are invalid:

```c
VkViewport viewport = {
    .x = 0.0f, .y = 0.0f,
    .width = (float)renderer->width,
    .height = (float)renderer->height,
    .minDepth = 0.0f, .maxDepth = 1.0f,
};
vkCmdSetViewport(cmd, 0, 1, &viewport);

VkRect2D scissor = {
    .offset = { 0, 0 },
    .extent = { renderer->width, renderer->height },
};
vkCmdSetScissor(cmd, 0, 1, &scissor);
```

## 12.3 Plain draws feed contiguous vertices

`vkCmdDraw` pushes a contiguous run, with `gl_VertexIndex` counting from `firstVertex`:

```c
void vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount,
    uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
```

Sources are bound first:

```c
void vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer,
    uint32_t firstBinding, uint32_t bindingCount,
    const VkBuffer *pBuffers, const VkDeviceSize *pOffsets);
```

Each slot pairs a `VERTEX_BUFFER` buffer with an offset; slot 0 matches binding 0. Keep pipeline, buffers, and shader `location`s in agreement.

Bind at offset zero, then draw three vertices, one instance, starting at vertex zero:

```c
struct Vertex { float x, y, z, w; float r, g, b, a; };

static const struct Vertex triangle[] = {
    { 0.0f,  1.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f, 1.0f },
    { 1.0f, -1.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f },
    { -1.0f, -1.0f, 0.0f, 1.0f,  0.0f, 1.0f, 0.0f, 1.0f },
};

VkDeviceSize offset = 0;
vkCmdBindVertexBuffers(cmd, 0, 1, &drawable->vertexBuf, &offset);
vkCmdDraw(cmd, 3, 1, 0, 0);
```

The parameters read in order: vertex count, instance count, first vertex, first instance. For one copy, pass `instanceCount` 1 and zeros for the `first` values. For a split stream, bind the same buffer twice under two bindings with different offsets (positions at offset 0 on binding 0, colors further into the buffer on binding 1), so each attribute stream advances at its own stride.

> **Misconception:** "The draw call carries the pixels."
> No. `vkCmdDraw` carries counts and start indices only. The pixels come from the bound vertex buffer plus the bound pipeline shaders and state. An empty bind or a mismatched vertex-input layout draws garbage or nothing, no matter how correct the counts look.

## 12.4 Indexed draws share vertices

A square needs four unique vertices but six corners as two triangles; the index buffer stores each vertex once and lists the visit order. At scale, an unrolled cube (36 position-only vertices at 12 bytes each) costs 432 bytes versus 168 bytes indexed (8 vertices plus 36 16-bit indices), with cache reuse on top (the companion triangle/square vertices above are 32B position-plus-color, so their absolute totals differ but the sharing ratio holds):

```c
void vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer,
    VkDeviceSize offset, VkIndexType indexType);
void vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount,
    uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
    uint32_t firstInstance);
```

```c
static const uint16_t squareIndices[] = { 0, 3, 1, 3, 2, 1 };

vkCmdBindVertexBuffers(cmd, 0, 1, &drawable->vertexBuf, &offset);
vkCmdBindIndexBuffer(cmd, drawable->indexBuf, 0, VK_INDEX_TYPE_UINT16);
vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
```

The device reads `indexCount` values at `offset + firstIndex * sizeof(index)`, adds signed `vertexOffset`, and feeds the result to assembly. Match the type to the stored width (`UINT16`/`uint16_t`, `UINT32`/`uint32_t`); the 32-bit addition must not overflow. `maxDrawIndexedIndexValue` is guaranteed at least 2^24-1, so check it near 32-bit limits.

Two variants help at the edges. Index-only rendering binds no vertex buffer and decodes `gl_VertexIndex` in the shader. Primitive restart cuts strips and fans without extra draws: enable `primitiveRestartEnable` on a strip/fan topology, then write `0xFFFF`/`0xFFFFFFFF` as the cut sentinel. Default to indexed `TRIANGLE_LIST` and sort draws by pipeline, set, then vertex buffer; cost tracks state changes, not triangles.

## 12.5 Instancing and indirect draws scale repeats

`instanceCount` runs the same vertices repeatedly while `gl_InstanceIndex` counts from `firstInstance`. Vary each copy by indexing an array with that value, or by adding a binding with `VK_VERTEX_INPUT_RATE_INSTANCE` that steps once per copy:

```glsl
layout (set = 0, binding = 0) uniform Transforms { mat4 mvp[256]; };
layout (set = 0, binding = 1) uniform Colors { vec4 color[256]; };
layout (location = 0) in vec3 i_position;
layout (location = 0) out vec4 v_color;
void main(void)
{
    v_color = color[gl_InstanceIndex];
    gl_Position = mvp[gl_InstanceIndex] * vec4(i_position, 1.0f);
}
```

```c
vkCmdDraw(cmd, 6, thumbCount, 0, 0); /* one TRIANGLE_LIST quad */
```

Use instancing for exact repeats differing only by matrix, color, or rectangle.

Direct draws bake counts into the command buffer. Indirect draws read them from a buffer at execution time, filled late or written by the device (`vkCmdFillBuffer`, `vkCmdCopyBuffer`, compute shader):

```c
void vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
    VkDeviceSize offset, uint32_t drawCount, uint32_t stride);
typedef struct VkDrawIndirectCommand {
    uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
} VkDrawIndirectCommand;
typedef struct VkDrawIndexedIndirectCommand {
    uint32_t indexCount, instanceCount, firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;
} VkDrawIndexedIndirectCommand;
```

Structures start at `offset`, spaced `stride` apart; stride zero replays one entry. A zero count skips that entry, giving a fixed table with a variable live prefix. Counts above one need `multiDrawIndirect` at `vkCreateDevice`, capped by `maxDrawIndirectCount`. `gl_DrawIDARB` (`GL_ARB_shader_draw_parameters`, guarded by `#extension GL_ARB_shader_draw_parameters : require`; check toolchain support) numbers subdraws for per-draw lookup. Prefer indirect draws for culled batches; direct draws debug more easily.

## 12.6 Secondaries split long passes

Record each chunk as a secondary with pass-continue inheritance, then execute it from the primary on the same subpass:

```c
VkCommandBufferInheritanceInfo inherit = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL,
    renderPass, 0, framebuffer, VK_FALSE, 0, 0
};
VkCommandBufferBeginInfo secBegin = {
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
    VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &inherit
};
vkBeginCommandBuffer(secondary, &secBegin);
vkCmdBindPipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
vkCmdDrawIndexed(secondary, 6, 1, 0, 0, 0);
vkEndCommandBuffer(secondary);

vkCmdBeginRenderPass(primary, &beginInfo,
    VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
vkCmdExecuteCommands(primary, 1, &secondary);
vkCmdEndRenderPass(primary);
```

Three rules: secondary `subpass` matches the primary's; `NULL` framebuffer is valid only inside one primary instance; the primary begins with `SECONDARY_COMMAND_BUFFERS` or execution trips validation. Secondaries are never submitted directly; split by layer or tile.

## 12.7 The frame loop: acquire, submit, present

Record once per image; each frame runs acquire, submit, present on two semaphores plus one per-frame fence named `frameFence` (semaphores order queue work, fences order host):

```c
VkSemaphoreCreateInfo semInfo = {
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    .pNext = NULL,
    .flags = 0,
};
vkCreateSemaphore(device, &semInfo, NULL, &drawable->imageAvailable);
vkCreateSemaphore(device, &semInfo, NULL, &drawable->renderFinished);

VkFenceCreateInfo fenceInfo = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    .pNext = NULL,
    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
};
vkCreateFence(device, &fenceInfo, NULL, &drawable->frameFence);
```

Acquire asks which image may be written (`UINT64_MAX` waits indefinitely):

```c
uint32_t imageIndex = 0;
vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
    drawable->imageAvailable, VK_NULL_HANDLE, &imageIndex);
```

At least one of semaphore or fence must be valid; the index alone is not permission to write. Each call returns a `VkResult` that decides the next step:

| `VkResult` | Meaning | Action |
|---|---|---|
| `VK_SUCCESS` | Image ready | Submit and present |
| `VK_SUBOPTIMAL_KHR` | Usable but mismatched | Render, then rebuild (12.8) |
| `VK_ERROR_OUT_OF_DATE_KHR` | Mismatched | Skip submit, rebuild now |
| `VK_NOT_READY` | No image, timeout 0 | Retry; not an error |
| `VK_TIMEOUT` | No image in time | Retry; not an error |
| `VK_ERROR_SURFACE_LOST_KHR` | Surface gone | Recreate surface and swapchain |

Submit runs the matching pre-recorded buffer, waiting for the image and signalling completion. The per-frame `frameFence` is waited on and reset before each submit so the host does not overwrite an in-flight frame:

```c
vkWaitForFences(device, 1, &drawable->frameFence, VK_TRUE, UINT64_MAX);
vkResetFences(device, 1, &drawable->frameFence);

VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

VkSubmitInfo submit = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .pNext = NULL,
    .waitSemaphoreCount = 1,
    .pWaitSemaphores = &drawable->imageAvailable,
    .pWaitDstStageMask = &waitStage,
    .commandBufferCount = 1,
    .pCommandBuffers = &drawable->cmdBuffers[imageIndex],
    .signalSemaphoreCount = 1,
    .pSignalSemaphores = &drawable->renderFinished,
};
vkQueueSubmit(queue, 1, &submit, drawable->frameFence);
```

`pWaitDstStageMask` stalls color-attachment output until the image is truly free.

Present returns ownership to the presentation engine:

```c
VkPresentInfoKHR present = {
    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    .pNext = NULL,
    .waitSemaphoreCount = 1,
    .pWaitSemaphores = &drawable->renderFinished,
    .swapchainCount = 1,
    .pSwapchains = &swapchain,
    .pImageIndices = &imageIndex,
    .pResults = NULL,
};
vkQueuePresentKHR(queue, &present);
```

Presented contents survive until overwritten. Destroy both semaphores with `vkDestroySemaphore` and the per-frame fence with `vkDestroyFence` at shutdown.

## 12.8 Resize rebuilds swapchain-dependent objects

A resize invalidates swapchain, framebuffers, pass dimensions, viewport assumptions, depth image, and command buffers. Drain the device, then destroy and recreate in dependency order:

```c
vkDeviceWaitIdle(device);
destroyFramebuffers();
destroyCommandPool();
destroyPipeline();
destroyRenderpass();
destroySwapchain();
destroyDrawableVertexBuffer();
destroyDepthBuffer();
initialize(); // createCommandPool, buildSwapChainAndDepthImage, createVertexBuffer, createRenderPass, createFrameBuffer, createShaders, createPipelineStateManagement
prepare(); // re-records the drawing command buffers
```

Guard with `isResizing` so no acquire/submit runs mid-rebuild; window message and Wayland configure get the same response: idle, rebuild, record again. The pipeline cache outlives the swapchain and is reused across rebuilds, so it is intentionally absent from the destroy list. Per-object data travels via push constants or an indexed storage array.

### Recap

- Pass instance binds pass, framebuffer, area, per-attachment clears; only `STORE` survives.
- Order: begin, pipeline, sets, buffers, viewport/scissor, draw.
- `vkCmdDraw` streams contiguous vertices; indexed shares them via index buffer plus offset.
- Instancing varies copies by index or rate; multi-indirect needs `multiDrawIndirect`, subdraws via `gl_DrawIDARB`.
- Secondaries need pass-continue inheritance, matching subpass, `SECONDARY_COMMAND_BUFFERS` primary.
- Acquire, submit (`imageAvailable` to `renderFinished`), present; table decides rebuilds.
- Resize idles, destroys in order, re-initializes, re-records.

### Exercises

1. **Batch the cube and trace one frame.** Render the 8-vertex cube with one 36-index draw, log `imageIndex`, both semaphores, and each `VkResult` over three frames, and explain why early submit paints an owned image.
2. **Secondaries plus clean resize.** Record two pass-continue secondaries for subpass 0, execute from a `SECONDARY_COMMAND_BUFFERS` primary (try `INLINE` once for the error), then resize under `isResizing` with the 12.8 list and confirm clean present.
