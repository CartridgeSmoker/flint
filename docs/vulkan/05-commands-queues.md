# Chapter 5: Queues and Command Buffers

> **Objectives:**
> - Describe queue families and select one with the capabilities you need.
> - Request device queues at creation and retrieve them with `vkGetDeviceQueue`.
> - Create command pools and allocate primary and secondary command buffers.
> - Record commands with begin, append, and end.
> - Submit with `vkQueueSubmit` and track completion with fences.
> - Record from multiple threads and reuse buffers with resets.

A device idles until you hand it work. The handoff uses two objects: a **queue**, which executes, and a **command buffer**, which carries a recorded list of commands. This chapter connects them, from picking a queue family to knowing when work finishes.

## 1. Queue families describe what a queue can do

Each physical device exposes one or more **queue families**: sets of queues with identical capabilities, performance, and memory access. Queues inside one family are interchangeable apart from synchronization. Different families may differ in operations or speed even when their flags look similar.

Query families before creating the logical device, then pick by capability flags, never by index. Use the standard two-call query: ask for the count, allocate exactly that many entries with `malloc`, then fill them:

```c
uint32_t familyCount = 0;
vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, NULL);

VkQueueFamilyProperties *families = malloc(familyCount * sizeof *families);
vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families);

uint32_t graphicsFamily = UINT32_MAX;
for (uint32_t i = 0; i < familyCount; i++) {
    if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { graphicsFamily = i; break; }
}
free(families);
```

Check `queueFlags` for `VK_QUEUE_GRAPHICS_BIT`, `VK_QUEUE_COMPUTE_BIT`, `VK_QUEUE_TRANSFER_BIT`, or `VK_QUEUE_SPARSE_BINDING_BIT`. Many desktops expose one do-everything family; discrete GPUs and mobile chips often add a copy-only transfer family worth using for staging uploads. That transfer family becomes useful in the next chapter, when large uploads move off the graphics path.

## 2. Request queues at creation, then borrow them

Queues are declared at device creation, not created on demand. Each family you touch gets a `VkDeviceQueueCreateInfo` with a queue count and a priority per queue. Its `flags` field is reserved and stays zero, `pQueuePriorities` points to an array of `queueCount` floats, and you provide one `VkDeviceQueueCreateInfo` per family you use:

```c
float priority = 1.0f;
VkDeviceQueueCreateInfo queueInfo = { 0 };
queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
queueInfo.queueFamilyIndex = graphicsFamily;
queueInfo.queueCount = 1;
queueInfo.pQueuePriorities = &priority;

VkDeviceCreateInfo deviceInfo = { 0 };
deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
deviceInfo.queueCreateInfoCount = 1;
deviceInfo.pQueueCreateInfos = &queueInfo;

VkDevice device = VK_NULL_HANDLE;
VkResult result = vkCreateDevice(physicalDevice, &deviceInfo, NULL, &device);
if (result != VK_SUCCESS) { /* Handle failure. */ }
```

After creation, borrow the handle. `vkGetDeviceQueue` has no return code; a bad family or index yields `VK_NULL_HANDLE`:

```c
VkQueue graphicsQueue = VK_NULL_HANDLE;
vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
```

There is no destroy call for queues. Access to one `VkQueue` needs external synchronization, so funnel its submits through a mutex or a single submit thread.

## 3. Pools allocate, buffers record

Commands are recorded into **command buffers** allocated from **command pools**. A pool belongs to exactly one queue family, and its buffers submit only to queues in that family.

```c
VkCommandPoolCreateInfo poolInfo = { 0 };
poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
poolInfo.queueFamilyIndex = graphicsFamily;

VkCommandPool pool = VK_NULL_HANDLE;
VkResult result = vkCreateCommandPool(device, &poolInfo, NULL, &pool);
if (result != VK_SUCCESS) { /* Handle failure. */ }
```

Two flags shape pool behavior. `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` marks buffers as short-lived so the driver picks a fragmentation-resistant strategy. `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` permits resetting single buffers; without it only a whole-pool reset recycles them. Use both for per-frame recording, neither for long-lived setup buffers.

Allocation names the pool, the level, and the count. Vulkan sizes buffers internally, so pass the count explicitly and never derive it with a `sizeof` expression over handles:

```c
VkCommandBufferAllocateInfo allocInfo = { 0 };
allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
allocInfo.commandPool = pool;
allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
allocInfo.commandBufferCount = 1;

VkCommandBuffer cmd = VK_NULL_HANDLE;
VkResult result = vkAllocateCommandBuffers(device, &allocInfo, &cmd);
if (result != VK_SUCCESS) { /* Handle failure. */ }
```

Return single buffers with `vkFreeCommandBuffers` or free everything with `vkDestroyCommandPool`. Never destroy a pool holding buffers still pending on a queue; drain the queue first.

## 4. Primary buffers submit; secondary buffers are called

Primary buffers submit directly to a queue and may call secondaries via `vkCmdExecuteCommands`. Secondary buffers never submit alone; they run inside a primary, inheriting render-pass state through `VkCommandBufferInheritanceInfo`.

Start with primaries only. Secondaries pay off when independent chunks combine into one submission: a scene pass plus an interface pass, or per-thread chunks joined by a stitching primary. Render-pass inheritance arrives in Chapter 12 (VkCommandBufferInheritanceInfo).

> **Flint note:** Flint records the 3D viewport and the UI overlay as two primaries on the graphics family, submitted in order each frame. If UI recording moves to workers, each worker gets its own pool and records a secondary, and the frame primary only stitches them. No transfer-queue split yet; uploads ride staging buffers on the graphics queue until profiling justifies more.

## 5. Recording: begin, append, end

Recording appends; nothing executes. `vkBeginCommandBuffer` resets the buffer and declares how it will be used:

```c
VkCommandBufferBeginInfo beginInfo = { 0 };
beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

VkResult result = vkBeginCommandBuffer(cmd, &beginInfo);
if (result != VK_SUCCESS) { /* Handle failure. */ }
```

`VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` promises one submission before reset and helps the driver. `VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT` allows overlapping executions; without it, never resubmit a buffer still pending. Zero means reusable but non-overlapping. Leave `pInheritanceInfo` as `NULL` for primaries; it applies to secondaries only.

Record a copy with `vkCmdCopyBuffer`. Offsets are relative to each buffer, not to the bound memory behind it:

```c
VkBufferCopy region = { 0 };
region.srcOffset = srcOffset;
region.dstOffset = dstOffset;
region.size = size;
vkCmdCopyBuffer(cmd, srcBuffer, dstBuffer, 1, &region);
```

The source need not hold valid data at record time, only before and during execution, and the destination must not be read until execution completes. Close with `vkEndCommandBuffer`, which finalizes the buffer for submission:

```c
VkResult result = vkEndCommandBuffer(cmd);
if (result != VK_SUCCESS) { /* Buffer not ready. */ }
```

> **Misconception:** "Calling vkCmd* runs the command now."
>
> It only appends to the buffer. That is why you can record a copy before the upload lands, and why reading the destination right after recording races the GPU. Execution starts at `vkQueueSubmit`, on the device schedule, and completion is reported by fences and semaphores, never by the return of the recording call.

## 6. Submission and completion

`vkQueueSubmit` hands finished buffers to a queue. Each `VkSubmitInfo` lists buffers plus optional semaphore waits and signals, covered in Chapter 15:

```c
VkSubmitInfo submit = { 0 };
submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
submit.commandBufferCount = 1;
submit.pCommandBuffers = &cmd;

VkFenceCreateInfo fenceInfo = { 0 };
fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

VkFence fence = VK_NULL_HANDLE;
VkResult result = vkCreateFence(device, &fenceInfo, NULL, &fence);
if (result != VK_SUCCESS) { /* Handle failure. */ }

result = vkQueueSubmit(graphicsQueue, 1, &submit, fence);
if (result != VK_SUCCESS) { /* Not scheduled; fence unsignaled. */ }
```

The fence signals when the submission completes. Wait, then reset it for reuse:

```c
vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
vkResetFences(device, 1, &fence);
```

`vkQueueSubmit` returns once work is scheduled, not once it runs. Buffers within one submit execute in order; separate submits and queues may overlap. `vkQueueWaitIdle` and `vkDeviceWaitIdle` drain a queue or the whole device, but they flush everything and stall the pipeline. Reserve them for shutdown and teardown; use fences and semaphores on the hot path.

## 7. Threads share nothing; reuse with resets

Recording scales, submitting serializes. Give each recording thread its own pool so no two threads touch the same pool or buffer, join the workers, then submit from one thread. Reset a frame pools only after that frame fence signals, so you never recycle a buffer the device still reads.

Rebuilding every frame works, but resetting reuses driver-side backing store. Reset one buffer when its pool allows individual resets:

```c
vkResetCommandBuffer(cmd, 0);
```

or recycle the whole pool at end of frame:

```c
vkResetCommandPool(device, pool, 0);
```

The `RELEASE_RESOURCES` bits (`VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT`, `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT`) also return backing store to the pool or system, at reallocation cost. Buffers grow toward their largest recording, so keep each buffer on a steady diet of similar work or periodically pass the release bit; otherwise one giant frame inflates every later one. Omit the release bit when the buffer will be re-recorded immediately so the driver can reuse its storage.

Commands alone move nothing until they reference real resources. Chapter 6 puts this machinery to work moving data between host-visible staging buffers and device-local memory, and Chapter 15 completes the picture with fences, semaphores, and frame-overlap ordering.

## Recap

- Families group identical queues; pick by `queueFlags`, not index, with a count-then-fill query.
- Declare queues at device creation, borrow handles with `vkGetDeviceQueue`, synchronize each queue externally.
- Pools bind to one family; `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` favors short-lived buffers, `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` enables single-buffer resets.
- Primaries submit and call secondaries; secondaries run only inside a primary.
- Recording appends: begin, `vkCmd*` calls, end. Nothing runs until submit.
- `vkQueueSubmit` schedules batches and optionally signals a fence; wait and reset the fence per frame. Idle calls are for shutdown.
- One pool per recording thread, one submit path per queue, reset after the fence, release resources when sizes vary.

## Exercises

1. **Copy with a fence, no idle call.** Record `vkCmdCopyBuffer` between two host-visible buffers into one primary, submit with a fence, wait with `vkWaitForFences`, verify the bytes, then reset buffer and fence for a second run. Confirm validation stays silent with no `*WaitIdle` on the path.
2. **Split recording across two pools.** Create two pools for one family, record half a multi-region copy in a primary from each, and submit both in one `VkSubmitInfo`. Then reset with flags of zero and re-record with swapped roles. If one side occasionally records ten times the regions, when would you add the release-resources bit?
