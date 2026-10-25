# Chapter 15: Synchronization

> **Objectives:**
> - Use fences for host-waits-device completion without polling or idle calls.
> - Hand work between queues and presentation with binary semaphores.
> - Track sequenced work with timeline semaphore counters.
> - Delimit work inside one queue with events and pipeline barriers.
> - Express memory visibility with access and stage masks.
> - Treat layout transitions as synchronization, and run frames in flight safely.
>
> **Flint note:** Flint runs two frames in flight, each recording two primaries, viewport then UI overlay, submitted in order so the interface composites over the scene. Per-frame uniforms and staging are doubled for the same reason. On the Wayland side, presentation uses explicit synchronization (`zwp_linux_explicit_synchronization_v1` over `linux-dmabuf`): Vulkan signals a release point when rendering finishes and the compositor waits before scanning out. Keep the handshakes distinct: fences recycle host-side slots, semaphores order device work, the Wayland release recycles the compositor-side buffer.

Vulkan never waits unless told to. Submissions run on the device schedule, queues overlap freely, and a write in one stage stays invisible to later stages until published. Synchronization adds back just enough order: **fences** let the host wait for the device, **semaphores** order queues against each other, and **events, barriers, and layout transitions** order work inside a queue.

## 15.1 Fences: the host waits for the device

A fence signals when a submission finishes. Only the host waits on it. Pass one to `vkQueueSubmit`, then block with `vkWaitForFences`, which can sleep, unlike spinning on `vkGetFenceStatus`:

```c
VkFenceCreateInfo fenceInfo = { 0 };
fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

VkFence frameFence = VK_NULL_HANDLE;
vkCreateFence(device, &fenceInfo, NULL, &frameFence);
/* Elided: submit (a VkSubmitInfo) and graphicsQueue defined elsewhere. */
vkQueueSubmit(graphicsQueue, 1, &submit, frameFence);

vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX);
vkResetFences(device, 1, &frameFence);
```

Create the fence signaled so the first frame wait passes through with no special case. A signaled fence stays signaled until the host resets it, so never reuse a command buffer or overwrite staging data until its fence signals. `vkWaitForFences` takes `waitAll` (wait for every fence when `VK_TRUE`, any one when `VK_FALSE`) and a `timeout` in nanoseconds, returning `VK_TIMEOUT` on expiry, so `UINT64_MAX` waits indefinitely. Use `vkGetFenceStatus` only as a non-blocking probe. Reserve `vkQueueWaitIdle` and `vkDeviceWaitIdle` for shutdown.

## 15.2 Binary semaphores: queue-to-queue handoff

A binary semaphore pairs one signal with one wait, with no host-side wait. The standard chain is acquire, render, present: the swapchain signals `imageAvailable`, rendering waits on it and signals `renderFinished`, presentation waits on that:

```c
VkSemaphoreCreateInfo semInfo = { 0 };
semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
VkSemaphore imageAvailable = VK_NULL_HANDLE;
VkSemaphore renderFinished = VK_NULL_HANDLE;
vkCreateSemaphore(device, &semInfo, NULL, &imageAvailable);
vkCreateSemaphore(device, &semInfo, NULL, &renderFinished);

vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
    imageAvailable, VK_NULL_HANDLE, &imageIndex);

/* Elided: cmd, frameFence, graphicsQueue, swapchain, imageIndex, presentQueue defined elsewhere. */
VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
VkSubmitInfo submit = { 0 };
submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
submit.waitSemaphoreCount = 1;
submit.pWaitSemaphores = &imageAvailable;
submit.pWaitDstStageMask = &waitStage;
submit.commandBufferCount = 1;
submit.pCommandBuffers = &cmd;
submit.signalSemaphoreCount = 1;
submit.pSignalSemaphores = &renderFinished;
vkQueueSubmit(graphicsQueue, 1, &submit, frameFence);

VkPresentInfoKHR present = { 0 };
present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
present.waitSemaphoreCount = 1;
present.pWaitSemaphores = &renderFinished;
present.swapchainCount = 1;
present.pSwapchains = &swapchain;
present.pImageIndices = &imageIndex;
vkQueuePresentKHR(presentQueue, &present);
```

Each `VkSubmitInfo` walks the handoff explicitly: `pWaitSemaphores` plus `pWaitDstStageMask` gate each wait on a pipeline stage, `pCommandBuffers` carries the work, `pSignalSemaphores` fires on completion, and the fence passed to `vkQueueSubmit` reports to the host. `pWaitDstStageMask` sets where the wait bites. Waiting at `COLOR_ATTACHMENT_OUTPUT_BIT` lets earlier vertex work overlap the wait instead of stalling at the top of the pipe. There is no reset call; design the signal-wait pairing first, then allocate objects around it. Sparse resources bind separately with `vkQueueBindSparse` rather than riding `VkSubmitInfo`.

## 15.3 Timeline semaphores: a counter instead of a bag of objects

When compute, transfer, and several frames join in, binary objects multiply. A **timeline semaphore** replaces them with one 64-bit counter. Timeline semaphores postdate the binary-only model of the era this guide draws on, and have been core since Vulkan 1.2 (shipped first as `VK_KHR_timeline_semaphore`). Submissions signal a value through `VkTimelineSemaphoreSubmitInfo`; waiters block until the counter reaches it. The host can also wait, signal, and query directly:

```c
VkSemaphoreTypeCreateInfo typeInfo = { 0 };
typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

VkSemaphoreCreateInfo semInfo = { 0 };
semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
semInfo.pNext = &typeInfo;
VkSemaphore frameCounter = VK_NULL_HANDLE;
vkCreateSemaphore(device, &semInfo, NULL, &frameCounter);

uint64_t signalValue = 7;
VkTimelineSemaphoreSubmitInfo tlInfo = { 0 };
tlInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
tlInfo.signalSemaphoreValueCount = 1;
tlInfo.pSignalSemaphoreValues = &signalValue;

VkSubmitInfo submit = { 0 };
submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
submit.pNext = &tlInfo;
submit.commandBufferCount = 1;
submit.pCommandBuffers = &cmd;
submit.signalSemaphoreCount = 1;
submit.pSignalSemaphores = &frameCounter;
vkQueueSubmit(computeQueue, 1, &submit, VK_NULL_HANDLE);

VkSemaphoreWaitInfo waitInfo = { 0 };
waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
waitInfo.semaphoreCount = 1;
waitInfo.pSemaphores = &frameCounter;
waitInfo.pValues = &signalValue;
vkWaitSemaphores(device, &waitInfo, UINT64_MAX);
```

Prefer timelines when ordering has a sequence number: frame N done, chunk N uploaded. Keep binaries for acquire-present, where the swapchain still speaks binary. The type is fixed at creation.

## 15.4 Events: fine-grained marks inside one stream

**Events** mark points inside one queue command stream (commonly one command buffer): set after the producer, wait before the consumer. The host can set with `vkSetEvent` or probe with `vkGetEventStatus`, but cannot block efficiently, so host-set events inject outside information rather than report completion. Reset device-side with `vkCmdResetEvent` or host-side with `vkResetEvent`; `vkGetEventStatus` returns `VK_EVENT_SET` or `VK_EVENT_RESET`, and host calls on an event require external synchronization, so guard them against concurrent access:

```c
VkEventCreateInfo eventInfo = { 0 };
eventInfo.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
VkEvent uploadDone = VK_NULL_HANDLE;
vkCreateEvent(device, &eventInfo, NULL, &uploadDone);

vkCmdSetEvent(cmd, uploadDone, VK_PIPELINE_STAGE_TRANSFER_BIT);
vkCmdWaitEvents(cmd, 1, &uploadDone,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
    0, NULL, 0, NULL, 0, NULL);
```

`vkCmdWaitEvents` also takes barriers, so one call adds execution and memory dependencies together. Still, a plain `vkCmdPipelineBarrier` covers most same-queue cases with less tracking. Reach for events when signal and wait sit far apart in a long buffer.

> **Misconception:** "If vkQueueSubmit returned, the work is done."
>
> It only scheduled the work. The staging buffer may still be in use and the image may still be rendering. Submit-return proves the driver accepted the batch; fences prove it finished, semaphores let a peer proceed, and barriers make writes visible. Reusing anything on submit-return is the fastest route to corrupting a frame that otherwise looked correct.

## 15.5 Pipeline barriers: execution order plus memory visibility

Submit order alone is not enough: Vulkan may reorder commands and keep writes in caches invisible to later readers. A **pipeline barrier** adds an *execution dependency* (later stages wait for earlier ones) and a *memory dependency* (source-access writes become visible to destination-access reads):

```c
VkMemoryBarrier barrier = { 0 };
barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;

vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
    0, 1, &barrier, 0, NULL, 0, NULL);
```

Keep masks narrow: transfer-write to vertex-input for an uploaded vertex buffer, shader-write to shader-read for compute-then-graphics. `VkBufferMemoryBarrier` targets one buffer and can transfer queue-family ownership via `srcQueueFamilyIndex` and `dstQueueFamilyIndex`. `VkImageMemoryBarrier` does the same for images and adds a layout transition. Set both family indices to `VK_QUEUE_FAMILY_IGNORED` when ownership stays put, or to the losing and gaining families to hand the resource over alongside the barrier.

## 15.6 Image layout transitions are synchronization

Each image use expects a layout: `TRANSFER_DST_OPTIMAL` for uploads, `SHADER_READ_ONLY_OPTIMAL` for sampling, `COLOR_ATTACHMENT_OPTIMAL` for rendering, `PRESENT_SRC_KHR` for display. Changes ride an image barrier inside the same dependency as the access masks, so a missing transition fails like a missing barrier:

```c
VkImageMemoryBarrier toRender = { 0 };
toRender.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
toRender.srcAccessMask = 0;
toRender.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
toRender.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
toRender.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
toRender.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
toRender.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
toRender.image = colorImage;
toRender.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
toRender.subresourceRange.levelCount = 1;
toRender.subresourceRange.layerCount = 1;

vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    0, 0, NULL, 0, NULL, 1, &toRender);
```

`UNDEFINED` as the old layout discards contents, which suits a fresh render target. Every swapchain image needs a transition to `PRESENT_SRC_KHR` before present. Each frame, name the current layout, the needed layout, and the access masks bridging them.

## 15.7 Host-device ordering and frames in flight

Host writes through a mapped pointer obey the same rules. Without `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`, call `vkFlushMappedMemoryRanges` after writing and `vkInvalidateMappedMemoryRanges` before reading, then add a device-side barrier so the bytes are visible at the consuming stage. Coherent memory skips the flush but still needs the barrier. Either way, treat a range referenced by a submitted buffer as lent to the device until its fence signals.

Two frames in flight keeps both sides busy: while frame N renders, the host records frame N+1, each frame owning its buffers, fence, and semaphore pair. The fence guards host reuse; the semaphores guard the device handoff:

```c
#define MAX_FRAMES_IN_FLIGHT 2
/* Elided: imageAvailable, renderFinished, swapchain, imageIndex defined elsewhere. */
VkFence frameFences[MAX_FRAMES_IN_FLIGHT];
uint32_t frame = 0;

for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    VkFenceCreateInfo info = { 0 };
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device, &info, NULL, &frameFences[i]);
}

vkWaitForFences(device, 1, &frameFences[frame], VK_TRUE, UINT64_MAX);
vkResetFences(device, 1, &frameFences[frame]);
vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
    imageAvailable[frame], VK_NULL_HANDLE, &imageIndex);
/* Record, submit signaling renderFinished[frame] with
   frameFences[frame], present waiting on renderFinished[frame]. */
frame = (frame + 1) % MAX_FRAMES_IN_FLIGHT;
```

Two bounds latency and memory while overlapping record and execution. Grow to three only when profiling shows starvation.

## Recap

- Fences report completion to the host; wait, then reset. Never poll in a spin or idle on the hot path.
- Binary semaphores pair one signal with one wait; pick the wait stage so earlier work overlaps.
- Timeline semaphores order sequenced work by counter value; keep binaries for acquire-present.
- Events mark points inside one buffer; barriers cover most same-queue cases.
- Barriers join execution and memory dependencies; keep stage and access masks narrow.
- Layout transitions ride image barriers; name old layout, new layout, and both access masks.
- Flush plus barrier for non-coherent host writes; treat submitted ranges as lent until the fence signals.
- Two frames in flight, each with its own buffers, fence, and semaphores, overlaps host and device.

## Exercises

1. **Fence-guarded ring upload.** Build a staging buffer with three segments and one fence per segment, each created signaled. Each frame, wait on the current fence, fill the segment, reset the fence, record `vkCmdCopyBuffer`, and submit with that fence. Confirm validation silence, then remove the wait and describe which corruption appears and why the other segments stay clean.
2. **Acquire-render-present trace.** Render a solid color using per-frame `imageAvailable` and `renderFinished` plus a per-frame fence. Move the wait stage from `TOP_OF_PIPE` to `COLOR_ATTACHMENT_OUTPUT_BIT` and state what overlaps. Then present waiting on the wrong frame semaphore and note whether you get a validation error, hang, or stale frame, and which primitive should have caught it.

Chapter 16 times readback with fences and query pools. Chapter 19 puts these primitives to work across queues and threads without data races. Chapter 7 returns to the present loop and places each fence wait and semaphore handoff in a running frame.
