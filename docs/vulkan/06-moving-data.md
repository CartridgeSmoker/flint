# Chapter 6: Moving Data

> **Objectives:**
> - Explain why copies, fills, and layout transitions are recorded commands, not host calls.
> - Move an image between layouts with `vkCmdPipelineBarrier` and an image memory barrier.
> - Order global and single-buffer hazards with `VkMemoryBarrier` and `VkBufferMemoryBarrier`.
> - Copy buffer regions with `VkBufferCopy` and choose transfer source and destination usage flags.
> - Upload through a host-visible staging buffer into device-local memory.
> - Copy between buffers and images with `VkBufferImageCopy`, and between images with `VkImageCopy` or `vkCmdBlitImage`.
> - Fill and update buffers with `vkCmdFillBuffer` and `vkCmdUpdateBuffer`, and clear images with `vkCmdClearColorImage`.

Chapter 4 backed resources with memory and Chapter 5 records commands. This chapter puts the first real work into those commands: moving bytes. The host can never touch device-local memory directly, so every upload, fill, and layout transition is a command the device executes after submission. Chapter 7 then takes images you have filled here and puts them on screen.

## 1. Copies run on the device, after you submit

`memcpy` runs when you call it. `vkCmdCopyBuffer` runs when the queue reaches it, possibly long after your recording function returned. Two consequences follow.

First, sources must stay alive until execution finishes, not just until recording returns. Do not destroy or reuse a source buffer or image until a fence or `vkQueueWaitIdle` confirms the submission completed. `vkCmdUpdateBuffer` is the exception on the host side: the driver snapshots your pointer at record time, so host memory may be reused immediately, though the destination buffer is still written late.

Second, ordering needs explicit barriers. Two copies in one command buffer run in submission order, but a shader read recorded after a copy still needs a barrier before the shader starts.

## 2. Images have layouts, and barriers move them

A buffer is flat bytes. An image is an opaque, possibly tiled arrangement the driver organizes per use. That organization is the **layout** (`VK_IMAGE_LAYOUT_UNDEFINED`, `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`, `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL`, `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`, `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`, `VK_IMAGE_LAYOUT_GENERAL`, and others). Each command accepts only certain layouts, and drawing in the wrong layout is undefined.

A **barrier** recorded with `vkCmdPipelineBarrier` both makes earlier writes visible to later reads and reorganizes image data across layouts. Name the producing and consuming stages plus the access types on each side:

```c
VkImageMemoryBarrier barrier = { 0 };
barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
barrier.image = textureImage;
barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
barrier.subresourceRange.levelCount = 1;
barrier.subresourceRange.layerCount = 1;

vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
    0, 0, NULL, 0, NULL, 1, &barrier);
```

This says transfer writes complete before fragment-shader reads, flushing caches as needed and retiling into shader-readable form. Pair stages and accesses honestly: `VK_PIPELINE_STAGE_TRANSFER_BIT` with `VK_ACCESS_TRANSFER_WRITE_BIT` into `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` with `VK_ACCESS_SHADER_READ_BIT` waits for exactly what happened. `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` also works but forces the widest stall, so keep it for prototyping.

`VK_IMAGE_LAYOUT_UNDEFINED` as `oldLayout` means previous contents may be discarded, which is correct for a fresh image about to receive an upload. `VK_QUEUE_FAMILY_IGNORED` on both family indices keeps ownership where it is; set real indices only when handing an image between queue families.

The `subresourceRange` selects which mips and layers the transition covers: `baseMipLevel` with `levelCount` and `baseArrayLayer` with `layerCount`, where `VK_REMAINING_MIP_LEVELS` and `VK_REMAINING_ARRAY_LAYERS` mean the rest of the image. A depth image sets `aspectMask` to `VK_IMAGE_ASPECT_DEPTH_BIT`, adding `VK_IMAGE_ASPECT_STENCIL_BIT` when the format carries stencil (the `S8` variants such as `VK_FORMAT_D24_UNORM_S8_UINT`). Inside a render pass, attachments transition implicitly through `initialLayout`, `finalLayout`, and subpass dependencies.

## 3. The other two barrier structs

`VkImageMemoryBarrier` is one of three. `VkMemoryBarrier` carries only access masks for global ordering with no resource attached, such as publishing a compute-shader write for a host readback:

```c
VkMemoryBarrier memBarrier = { 0 };
memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
memBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0, 1, &memBarrier, 0, NULL, 0, NULL);
```

`VkBufferMemoryBarrier` scopes the hazard to one buffer range through `buffer`, `offset`, and `size`, where `VK_WHOLE_SIZE` selects the whole buffer, and it transfers ownership by naming real family indices instead of `VK_QUEUE_FAMILY_IGNORED`:

```c
VkBufferMemoryBarrier bufBarrier = { 0 };
bufBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
bufBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
bufBarrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
bufBarrier.srcQueueFamilyIndex = transferFamilyIndex;
bufBarrier.dstQueueFamilyIndex = graphicsFamilyIndex;
bufBarrier.buffer = vertexBuffer;
bufBarrier.offset = 0;
bufBarrier.size = VK_WHOLE_SIZE;

vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
    0, 0, NULL, 1, &bufBarrier, 0, NULL);
```

Pass `VK_DEPENDENCY_BY_REGION_BIT` in `dependencyFlags` when the hazard is framebuffer-local, letting the implementation synchronize per region instead of stalling globally.

> **Misconception:** "Layouts are hints and barriers are flushes I can skip on simple GPUs."
>
> No. On some drivers the layout changes nothing and the missing barrier appears harmless. On tiled or compressed architectures the same program shows noise or stale data, and validation layers flag the transition. Record every transition and every write-then-read handoff with the real stages and accesses; on forgiving drivers the honest barrier is cheap, and on strict drivers it is the only correct program.

## 4. Buffer copies and regions

The simplest transfer copies bytes between buffers. One call copies many regions through an array of `VkBufferCopy`:

```c
VkBufferCopy region = { 0 };
region.srcOffset = 0;
region.dstOffset = 0;
region.size = vertexDataSize;

vkCmdCopyBuffer(cmd, stagingBuffer, vertexBuffer, 1, &region);
```

Usage flags must cover the transfer: sources need `VK_BUFFER_USAGE_TRANSFER_SRC_BIT`, destinations need `VK_BUFFER_USAGE_TRANSFER_DST_BIT` plus the real use. A missing `VK_BUFFER_USAGE_TRANSFER_DST_BIT` is one of the most common validation errors in new code.

## 5. Staging: host-visible to device-local

Device-local memory is fast for the GPU and invisible to the host; host-visible memory is mappable and slower. Uploads use both: write into a host-visible **staging** buffer, then copy device-side into the final buffer.

```c
VkBufferCreateInfo stagingInfo = { 0 };
VkBufferCreateInfo targetInfo = { 0 };
stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
stagingInfo.size = dataSize;
stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

targetInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
targetInfo.size = dataSize;
targetInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
    | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
targetInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
```

Allocate host-visible coherent memory for staging and device-local memory for the target (Chapter 4), bind each, then map, write, and unmap:

```c
void *mapped = NULL;
vkMapMemory(device, stagingMemory, 0, dataSize, 0, &mapped);
memcpy(mapped, vertices, (size_t)dataSize);
vkUnmapMemory(device, stagingMemory);

VkBufferCopy stagingCopy = { 0 };
stagingCopy.srcOffset = 0;
stagingCopy.dstOffset = 0;
stagingCopy.size = dataSize;

vkCmdCopyBuffer(cmd, stagingBuffer, vertexBuffer, 1, &stagingCopy);
```

Keep staging alive until the submission fence signals, then destroy or recycle it. With a dedicated transfer queue family, record the copy there and add an ownership barrier before the graphics queue consumes the result.

> **Flint note:** Flint uploads glyph atlases and panel geometry this way: text shaping writes pixels on the CPU into staging, a transfer copies them into a device-local sampled image, and a barrier moves it to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` before the frame renders. Static geometry uploads once at load; per-frame dynamic data prefers mapped persistent buffers over one-shot staging.

## 6. Buffers to images, and back

Texture upload is `vkCmdCopyBufferToImage`. The image must sit in `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` (or `VK_IMAGE_LAYOUT_GENERAL`) and carry `VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`. Each `VkBufferImageCopy` pairs a buffer offset with an image subresource and rectangle:

```c
VkBufferImageCopy copy = { 0 };
copy.bufferOffset = 0; /* row length/height 0 means tightly packed */
copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
copy.imageSubresource.layerCount = 1;
copy.imageExtent.width = texWidth;
copy.imageExtent.height = texHeight;
copy.imageExtent.depth = 1;

vkCmdCopyBufferToImage(cmd, stagingBuffer, textureImage,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
```

A full upload is three commands: barrier `VK_IMAGE_LAYOUT_UNDEFINED` to `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL`, the copy, barrier to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. Readback mirrors it through `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` and `vkCmdCopyImageToBuffer`. Compressed formats add one rule: offsets and extents must be multiples of the block size (usually 4 by 4 texels) unless the region touches the edge of an image whose dimensions are not block multiples. Vulkan moves compressed blocks as opaque bits; it never compresses or decompresses for you.

## 7. Image to image: copies and blits

`vkCmdCopyImage` moves texels unchanged between images: source in `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` or `VK_IMAGE_LAYOUT_GENERAL`, destination in `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` or `VK_IMAGE_LAYOUT_GENERAL`, one shared extent per `VkImageCopy`:

```c
VkImageCopy region2 = { 0 };
region2.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
region2.srcSubresource.layerCount = 1;
region2.dstSubresource = region2.srcSubresource;
region2.extent.width = texWidth;
region2.extent.height = texHeight;
region2.extent.depth = 1;

vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region2);
```

Compressed pairs must share the same block extent (for example two 4 by 4 block formats); mixed compressed and uncompressed copies are allowed only when block byte sizes match. The shared `extent` is in source texels, so the destination footprint scales into matching blocks while moving identical byte counts. Offsets and extents stay aligned to each side's block extent except at the image edge.

For scaling, format conversion, or mirroring, use `vkCmdBlitImage`. The source must sit in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL or VK_IMAGE_LAYOUT_GENERAL and the destination in VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL or VK_IMAGE_LAYOUT_GENERAL. Each `VkImageBlit` names two corners per side so rectangles may differ in size, with `VK_FILTER_NEAREST` or `VK_FILTER_LINEAR` resampling; swapping the two corners on one side mirrors that axis. Check `VK_FORMAT_FEATURE_BLIT_SRC_BIT` and `VK_FORMAT_FEATURE_BLIT_DST_BIT` first: compressed destinations are rarely supported.

## 8. Fills, small updates, and clears

Small writes need no staging buffer:

```c
/* Repeat one 32-bit word. Offset and size are bytes, multiples of 4. */
vkCmdFillBuffer(cmd, dstBuffer, 0, VK_WHOLE_SIZE, 0u);

/* Inline up to 64 KiB; the host pointer is consumed at record time. */
vkCmdUpdateBuffer(cmd, dstBuffer, 0,
    (VkDeviceSize)sizeof(params), (const uint32_t *)&params);

/* Clear a color image in VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL or VK_IMAGE_LAYOUT_GENERAL. */
VkClearColorValue clear = { { 0.0f, 0.0f, 0.0f, 1.0f } };
VkImageSubresourceRange clearRange = { 0 };
clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
clearRange.levelCount = 1;
clearRange.layerCount = 1;

vkCmdClearColorImage(cmd, targetImage,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &clearRange);
```

A float fill reinterprets the bits of the word you pass. `vkCmdUpdateBuffer` suits small uniform writes; anything larger belongs in staging plus a copy. `vkCmdClearColorImage` reads the union member matching the image format; depth-stencil images use `vkCmdClearDepthStencilImage` instead. All three still need correct layouts and a barrier before any later read of the result.

## Recap

- Copies, fills, clears, and transitions are device commands run at submission time: sources must outlive the submission and ordering needs barriers.
- Images must sit in a layout each command accepts; barriers change layouts and publish writes over an explicit subresource range.
- `VkMemoryBarrier` orders globally and `VkBufferMemoryBarrier` scopes to one buffer range and can transfer queue-family ownership.
- `vkCmdCopyBuffer` moves `VkBufferCopy` regions; both sides need transfer usage bits.
- Staging writes host-visible memory, copies device-side into device-local memory, then barriers into shader-readable layout: the Flint atlas path.
- Buffer-image copies share `VkBufferImageCopy` (zero row length means packed); compressed copies align to block size.
- `vkCmdCopyImage` moves texels unchanged; `vkCmdBlitImage` scales, converts, mirrors, and filters where format features allow.
- `vkCmdFillBuffer`, `vkCmdUpdateBuffer` (64 KiB limit), and `vkCmdClearColorImage` handle small writes without staging.

## Exercises

1. **Staged triangle upload.** Create a host-visible staging buffer and a device-local vertex buffer for three vertices. Map, write, unmap, record `vkCmdCopyBuffer`, submit, and wait on a fence before destroying staging resources. Then omit `VK_BUFFER_USAGE_TRANSFER_DST_BIT`, record the validation error, restore it, and confirm clean execution.
2. **Texture upload with two barriers.** Upload a 4 by 4 RGBA image through staging: barrier `VK_IMAGE_LAYOUT_UNDEFINED` to `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL`, `vkCmdCopyBufferToImage` tightly packed, barrier to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (`VK_ACCESS_TRANSFER_WRITE_BIT` to `VK_ACCESS_SHADER_READ_BIT`). Sample it, then swap the second barrier's stages for `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` and explain what the honest stages buy back.
