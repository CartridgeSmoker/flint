# Chapter 11: Render Passes and Framebuffers

> **Objectives:**
> - Create a vertex buffer and upload geometry with explicit teardown order.
> - Classify attachments into color, depth/stencil, resolve, input, and preserve roles.
> - Declare a render pass with load/store operations, layouts, and an external dependency.
> - Build one framebuffer per swapchain image with matching extents.
> - Record a clear-valued render pass instance inside the acquire, submit, and present loop.

Chapter 8 turned GLSL into shader modules and Chapter 10 froze those modules into pipelines. This chapter builds the surface those pipelines draw on: buffers holding geometry, a render pass declaring the job, and framebuffers binding real views to that declaration. Chapter 12 then records the full frame loop that drives all three.

## 1. Buffers hold the geometry

A `VkBuffer` is a one-dimensional byte array: vertices, indices, uniforms, or storage data. Like images, a buffer is born unbacked. `vkCreateBuffer` records size, usage, and sharing; a later allocation supplies the memory:

```c
VkBufferCreateInfo bufInfo = { 0 };
bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
bufInfo.size = dataSize;
bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
VkBuffer vertexBuf = VK_NULL_HANDLE;
vkCreateBuffer(device, &bufInfo, NULL, &vertexBuf);
```

`usage` is a bitmask and must list every future use, so combining bits such as vertex-buffer plus transfer-destination use is normal when the buffer later serves as a copy target. `SHARING_MODE_EXCLUSIVE` fits single-queue ownership. Teardown order matters: destroy the buffer first with `vkDestroyBuffer`, then free the bound memory with `vkFreeMemory`. Freeing memory while the buffer still references it, or destroying views the framebuffer still needs, is a use-after-free that validation layers catch.

Three interleaved vertices, each a four-component position plus a four-component color, show the upload pattern:

```c
struct VertexWithColor { float x, y, z, w; float r, g, b, a; };
static const struct VertexWithColor triangleData[] = {
    {  0.0f,  1.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f, 1.0f },
    {  1.0f, -1.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f },
    { -1.0f, -1.0f, 0.0f, 1.0f,  0.0f, 1.0f, 0.0f, 1.0f },
};
```

Upload follows query, allocate, map, copy, unmap, bind:

```c
VkMemoryRequirements reqs;
vkGetBufferMemoryRequirements(device, vertexBuf, &reqs);
uint32_t typeIndex = findMemoryType(physDevice, reqs.memoryTypeBits,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
VkMemoryAllocateInfo allocInfo = { 0 };
allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
allocInfo.allocationSize = reqs.size;
allocInfo.memoryTypeIndex = typeIndex;
VkDeviceMemory vertexMem = VK_NULL_HANDLE;
vkAllocateMemory(device, &allocInfo, NULL, &vertexMem);
void *mapped = NULL;
vkMapMemory(device, vertexMem, 0, reqs.size, 0, &mapped);
memcpy(mapped, triangleData, sizeof(triangleData));
vkUnmapMemory(device, vertexMem);
vkBindBufferMemory(device, vertexBuf, vertexMem, 0);
```

Host-visible plus coherent memory keeps this upload simple with no explicit flushes. Large meshes later use device-local memory with a staging buffer; the five calls keep the same shape. The pipeline from Chapter 10 takes stride and attribute shapes at creation:

```c
VkVertexInputBindingDescription bindDesc = { 0, sizeof(struct VertexWithColor),
    VK_VERTEX_INPUT_RATE_VERTEX };
VkVertexInputAttributeDescription attrDesc[2] = {
    { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
    { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
};
```

Binding 0 advances per vertex with a 32-byte stride. Location 0 reads position at offset 0; location 1 reads color at offset 16. An index buffer repeats the pattern with `VK_BUFFER_USAGE_INDEX_BUFFER_BIT`, bound later with `vkCmdBindIndexBuffer` and drawn with `vkCmdDrawIndexed`.

> **Flint note:** Flint follows this chapter exactly for its first triangle: host-visible vertex upload, one-subpass render pass over the swapchain color view plus depth view, and one framebuffer per image.

## 2. Attachments come in five roles

A render pass is the contract for one job: which attachments exist, how each starts and ends, and which subpasses use them. Attachments fall into five roles: color, depth/stencil, resolve, input, and preserve. Color and depth/stencil are written during the subpass, resolve receives the multisample downsampling result, input attachments let a later subpass read an earlier subpass result without leaving the pass, and preserve lists attachments kept alive across a subpass without being accessed. On tiler GPUs this structure matters because the pass boundary tells the driver when tile memory can be flushed to main memory.

Each attachment is a `VkAttachmentDescription` with format, sample count, load and store behavior, and entry and exit layouts:

```c
VkAttachmentDescription attachments[2] = { { 0 } };
attachments[0].format = swapchainFormat;
attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
attachments[1].format = depthFormat;
attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
```

`CLEAR` wipes the render area with a caller-supplied value; `LOAD` keeps prior contents; `STORE` keeps the result while `DONT_CARE` lets the driver discard it. Here `CLEAR` plus `STORE` keeps the color result for presentation, while the transient depth result uses `DONT_CARE` on store. Starting from `UNDEFINED` is legal because both attachments clear first, which lets the driver skip loading old contents. Ending color in `PRESENT_SRC_KHR` hands it to the presentation engine without a further barrier. If multisampling, pair each multisample color attachment (`STORE` of `DONT_CARE`) with a single-sample resolve attachment (`STORE` of `STORE`); with one sample the resolve reference stays NULL.

> **Misconception:**
>
> "Vertex buffers need a buffer view before the pipeline can read them, and clearing needs a draw." Neither is true. Fixed-function vertex fetch reads `VkBuffer` bindings directly through the binding and attribute descriptions above; views created with `vkCreateBufferView` serve only shaders reading buffer bytes as formatted texels. Clearing likewise needs no draw: a render pass instance whose load operation is `CLEAR` wipes the attachments when the pass begins.

## 3. One subpass plus an external edge

One subpass ties the attachments together:

```c
VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
VkSubpassDescription subpass = { 0 };
subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
subpass.colorAttachmentCount = 1;
subpass.pColorAttachments = &colorRef;
subpass.pDepthStencilAttachment = &depthRef;
```

A minimal example runs with `dependencyCount = 0`, but production code adds a `VK_SUBPASS_EXTERNAL` dependency synchronizing color-attachment output with the acquire and present handoffs, so layout transitions and memory visibility cover the image edge:

```c
VkSubpassDependency edge = { 0 };
edge.srcSubpass = VK_SUBPASS_EXTERNAL;
edge.dstSubpass = 0;
edge.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
edge.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
edge.srcAccessMask = 0;
edge.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
VkRenderPassCreateInfo passInfo = { 0 };
passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
passInfo.attachmentCount = 2;
passInfo.pAttachments = attachments;
passInfo.subpassCount = 1;
passInfo.pSubpasses = &subpass;
passInfo.dependencyCount = 1;
passInfo.pDependencies = &edge;
VkRenderPass renderPass = VK_NULL_HANDLE;
vkCreateRenderPass(device, &passInfo, NULL, &renderPass);
```

Without that edge, the transition from `UNDEFINED` or `PRESENT_SRC_KHR` into `COLOR_ATTACHMENT_OPTIMAL`, and back out to present, has no ordering guarantee against the semaphore signals in the next section. Destroy the pass with `vkDestroyRenderPass` only after its framebuffers and pipelines are gone.

## 4. One framebuffer per swapchain image

A framebuffer binds concrete views to the pass attachments. Build one per image, pairing its color view with the shared depth view:

```c
VkFramebuffer *framebuffers = malloc(imageCount * sizeof(*framebuffers));
for (uint32_t i = 0; i < imageCount; ++i) {
    VkImageView views[2] = { colorViews[i], depthView };
    VkFramebufferCreateInfo fbInfo = { 0 };
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments = views;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;
    vkCreateFramebuffer(device, &fbInfo, NULL, &framebuffers[i]);
}
```

Attachment index 0 must match `colorRef` and index 1 must match `depthRef`. Width, height, and layers must agree with every view and stay within `maxFramebufferWidth`, `maxFramebufferHeight`, and `maxFramebufferLayers`. A render pass with no attachments pairs with an attachmentless framebuffer (`attachmentCount = 0`): no views are bound, but valid width, height, and layers are still required within the same `maxFramebuffer` limits. A pipeline built for that render pass stays compatible when formats, sample counts, and attachment roles agree; only the size and the actual images may differ, which is why one pipeline serves every framebuffer here. Destroy each entry with `vkDestroyFramebuffer` before destroying the pass or the views, typically at swapchain resize alongside the views.

## 5. Clear values and the frame loop

With the pass and framebuffers ready, clearing is a render pass instance with no draws inside: one command buffer per image holding `begin` and `end` around its framebuffer:

```c
VkClearValue clearValues[2];
clearValues[0].color.float32[0] = 0.0f;
clearValues[0].color.float32[1] = 0.0f;
clearValues[0].color.float32[2] = 0.0f;
clearValues[0].color.float32[3] = 1.0f;
clearValues[1].depthStencil.depth = 1.0f;
clearValues[1].depthStencil.stencil = 0;
VkRenderPassBeginInfo beginInfo = { 0 };
beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
beginInfo.renderPass = renderPass;
beginInfo.framebuffer = framebuffers[currentImage];
beginInfo.renderArea.offset.x = 0;
beginInfo.renderArea.offset.y = 0;
beginInfo.renderArea.extent.width = width;
beginInfo.renderArea.extent.height = height;
beginInfo.clearValueCount = 2;
beginInfo.pClearValues = clearValues;
vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
vkCmdEndRenderPass(cmd);
```

`clearValueCount` must cover every attachment, in attachment order. Tint per image by varying `clearValues[0]`; a handy smoke test clears successive images red, green, and blue, then displays each cleared image. Keep alpha at opaque 1.0f (matching Chapter 12) during the per-image tint test and vary only the RGB channels.

Around these command buffers runs the per-frame skeleton (full loop in Chapter 12), one recorded buffer per swapchain image:

```c
uint32_t image = 0;
vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
    acquireSem, VK_NULL_HANDLE, &image);
VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
VkSubmitInfo submit = { 0 };
submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
submit.waitSemaphoreCount = 1;
submit.pWaitSemaphores = &acquireSem;
submit.pWaitDstStageMask = &waitStage;
submit.commandBufferCount = 1;
submit.pCommandBuffers = &cmds[image];
submit.signalSemaphoreCount = 1;
submit.pSignalSemaphores = &releaseSem;
vkQueueSubmit(graphicsQueue, 1, &submit, frameFence);
VkPresentInfoKHR present = { 0 };
present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
present.waitSemaphoreCount = 1;
present.pWaitSemaphores = &releaseSem;
present.swapchainCount = 1;
present.pSwapchains = &swapchain;
present.pImageIndices = &image;
vkQueuePresentKHR(presentQueue, &present);
```

Acquire claims the next image, submission runs that image's command buffer synchronized with the acquire and release semaphores, and present displays it. The wait stage ties the acquire semaphore to color-attachment output, matching the external dependency above. The full synchronization chain, fences, and resize handling continue in Chapter 12.

---

### Recap

- `VkBuffer` is created unbacked; query, allocate host-visible coherent memory, map, copy, unmap, bind. Destroy the buffer before freeing its memory; vertex and index fetch need no view.
- Bindings give stride and rate while attributes give location, binding, format, and offset: stride 32, offsets 0 and 16 in the triangle example.
- Attachments take five roles (color, depth/stencil, resolve, input, preserve); each description fixes format, samples, load/store behavior, and entry/exit layouts.
- A render pass declares attachments and subpasses; one graphics subpass suffices here, with a `VK_SUBPASS_EXTERNAL` edge ordering the acquire and present handoffs.
- One framebuffer per image pairs its color view with the depth view in order, with matching extents; pipelines stay compatible across sizes.
- Clearing is a `vkCmdBeginRenderPass` and `vkCmdEndRenderPass` pair with one clear value per attachment, driven by the acquire, submit, and present skeleton.

### Exercises

1. **Upload and verify the triangle.** Create the vertex buffer above, bind host-visible coherent memory, and compare `reqs.size` with `sizeof(triangleData)`. Explain why the allocation may exceed the data, flip one color byte, predict which corner changes, then add an index buffer and draw with `vkCmdDrawIndexed`.
2. **Recolor the clear and cut the edge.** Clear image 0 red, 1 green, and 2 blue with depth 1.0, then acquire, submit, and present each. Drop `clearValueCount` to 1 while both attachments clear and record the validation message, then remove the external dependency and explain which acquire-to-draw ordering guarantee disappears.
