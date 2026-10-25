# Chapter 19: Multi-Threaded Recording

> **Objectives:** Explain why task-based parallelism beats one-thread-per-job; initialise enkiTS and submit lambda and pinned tasks; reserve a pinned I/O thread for async loads; select graphics and transfer queue families and create both queues; stage uploads through a mapped buffer with ownership-transfer barriers; size pools as frames times threads and recycle them; choose primary versus secondary buffers and record draws across tasks. Chapter 20 automates grouping by render pass, and Chapter 21 overlaps async work on dedicated queues.

Single-threaded rendering leaves cores idle and stalls the frame on file I/O. Vulkan, unlike OpenGL, expects threading: buffers recorded on different threads can submit together, provided each thread owns its command pool. This chapter adds a task scheduler, moves loading to the background, and splits recording across cores.

> **Flint note:** Flint wants exactly this split. Keep the UI thread free for input and layout while a pinned loader streams viewport textures and worker tasks record viewport command buffers. Never block UI on image decode or `vkQueueSubmit`.

## 19.1 Why tasks, not threads

One-thread-per-job (render thread, I/O thread) works on two cores, then stalls: jobs are uneven and threads idle. Task systems invert this. Work becomes small units that run on any worker thread, declare dependencies, and form an execution graph. Idle threads steal work from busy ones, keeping utilisation high without hand-placing threads.

Fibres, the alternative, may suspend mid-flight, which is flexible but hard to debug. This guide uses tasks via enkiTS.

## 19.2 Driving enkiTS

Initialise the scheduler once, then submit an `enki::ITaskSet`. The lambda form below is equivalent to subclassing and easier to read:

```cpp
enki::TaskSchedulerConfig config;
config.numTaskThreadsToCreate = 4;
enki::TaskScheduler scheduler;
scheduler.Initialize(config);

enki::TaskSet task(1, [](enki::TaskSetPartition range, uint32_t threadnum) {
    // work for range.start to range.end
});
scheduler.AddTaskSetToPipe(&task);
scheduler.WaitforTask(&task);
```

`AddTaskSetToPipe` enqueues; `WaitforTask` blocks while helping run other tasks. Running tasks may enqueue children, which land locally but stay stealable. Pinned tasks are the exception: they bind to one thread index for long-lived services such as I/O.

## 19.3 Async loading with a pinned I/O thread

Texture loads are slow: disk and decode dwarf RAM and cache speeds. Reserve the last scheduler thread for I/O with a loop task that pumps pinned work and a loader task that drains requests:

```cpp
struct RunPinnedTaskLoopTask : enki::IPinnedTask {
    void Execute() override {
        while (execute && pScheduler->GetIsRunning()) {
            pScheduler->WaitForNewPinnedTasks();
            pScheduler->RunPinnedTasks();
        }
    }
    enki::TaskScheduler* pScheduler = nullptr;
    bool execute = true;
};

struct AsyncLoadTask : enki::IPinnedTask {
    void Execute() override {
        while (execute)
            loader->update();
    }
    AsyncLoader* loader = nullptr;
    bool execute = true;
};

uint32_t ioThread = scheduler.GetNumTaskThreads() - 1;
runLoop.pScheduler = &scheduler;
runLoop.threadNum = ioThread;
scheduler.AddPinnedTask(&runLoop);
asyncLoad.loader = &loader;
asyncLoad.threadNum = ioThread;
scheduler.AddPinnedTask(&asyncLoad);
```

Decoding just pushes an upload request:

```cpp
int w = 0, h = 0, comp = 0;
uint8_t* pixels = stbi_load(req.path, &w, &h, &comp, 4);
UploadRequest& up = uploadQueue.push_use();
up.data = pixels;
up.texture = req.texture;
```

## 19.4 Vulkan queues and parallel submission

A `VkQueue` submits recorded buffers to the GPU. Recording scales across pools; submission through one queue object needs external synchronisation. Extra queue families give independent paths:

- `VK_QUEUE_GRAPHICS_BIT`: draws and render passes.
- `VK_QUEUE_COMPUTE_BIT`: dispatches and ray tracing.
- `VK_QUEUE_TRANSFER_BIT`: copies such as `vkCmdCopyBufferToImage`.

Pick a full-capability graphics family plus a compute-free transfer family:

```cpp
uint32_t count = 0;
vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
std::vector<VkQueueFamilyProperties> families(count);
vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, families.data());

uint32_t graphicsFamily = UINT32_MAX, transferFamily = UINT32_MAX;
for (uint32_t i = 0; i < count; ++i) {
    if (families[i].queueCount == 0)
        continue;
    constexpr VkQueueFlags kAll = VK_QUEUE_GRAPHICS_BIT |
        VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    if ((families[i].queueFlags & kAll) == kAll)
        graphicsFamily = i;
    if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0 &&
        (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT))
        transferFamily = i;
}
```

Request both at device creation (fall back to the graphics queue with a mutex if no transfer family exists):

```cpp
float priority = 1.0f;
VkDeviceQueueCreateInfo queues[2]{};
queues[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
queues[0].queueFamilyIndex = graphicsFamily;
queues[0].queueCount = 1;
queues[0].pQueuePriorities = &priority;
uint32_t queueCount = 1;
if (transferFamily != UINT32_MAX) {
    queues[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queues[1].queueFamilyIndex = transferFamily;
    queues[1].queueCount = 1;
    queues[1].pQueuePriorities = &priority;
    queueCount = 2;
}
VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
deviceInfo.queueCreateInfoCount = queueCount;
deviceInfo.pQueueCreateInfos = queues;
vkCreateDevice(physicalDevice, &deviceInfo, allocCallbacks, &device);

vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
if (transferFamily != UINT32_MAX)
    vkGetDeviceQueue(device, transferFamily, 0, &transferQueue);
else
    transferQueue = graphicsQueue;
```

> **Misconception: "Each thread needs its own queue."** Threads need their own pools, not queues. One graphics plus one transfer queue already overlaps uploads with drawing. Most devices expose a single graphics queue, and ordering work across more of them costs more than recording in parallel and submitting once.

## 19.5 Staging, fences, and ownership transfer

Uploads need a transfer pool, a mapped staging buffer, and a fence:

```cpp
VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
poolInfo.queueFamilyIndex = transferFamily;
poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
vkCreateCommandPool(device, &poolInfo, allocCallbacks, &transferPools[frame]);

VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
bufferInfo.size = 64u * 1024u * 1024u;
VmaAllocationCreateInfo vmaInfo{};
vmaInfo.flags = VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
VmaAllocationInfo allocOut{};
vmaCreateBuffer(allocator, &bufferInfo, &vmaInfo,
    &staging.buffer, &staging.allocation, &allocOut);
staging.mapped = static_cast<uint8_t*>(allocOut.pMappedData);
```

Each pump waits on the fence, copies into the staging ring, and records the copy with barriers. Offsets into the staging ring must stay aligned for texture copies, so bump the offset up to `k_texture_alignment` before writing:

```cpp
if (vkGetFenceStatus(device, transferFence) != VK_SUCCESS)
    return; // GPU busy; retry next pump
vkResetFences(device, 1, &transferFence);
stagingOffset = memory_align(stagingOffset, k_texture_alignment);
memcpy(staging.mapped + stagingOffset, req.data, imageSize);

VkImageMemoryBarrier pre{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
pre.srcAccessMask = 0;
pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
pre.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
pre.image = image;
pre.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &pre);

VkBufferImageCopy region{};
region.bufferOffset = stagingOffset;
vkCmdCopyBufferToImage(cmd, staging.buffer, image,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

VkImageMemoryBarrier post{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
post.dstAccessMask = 0;
post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
post.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
post.srcQueueFamilyIndex = transferFamily;
post.dstQueueFamilyIndex = graphicsFamily;
post.image = image;
post.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &post);
```

The main thread finishes with a graphics-queue barrier to `VK_ACCESS_SHADER_READ_BIT` before first sample. The transfer submission signals `transfer_complete_semaphore`, which the graphics queue waits on before first sample (semaphore wait elided from the snippets above for brevity). Ready textures travel through a mutex-protected list from loader to renderer.

## 19.6 Allocation strategy and recycling

Pools need external synchronisation, so never share one across threads. The robust rule is `F * T` graphics pools (`F` frames in flight, `T` recording threads), indexed by `(frame, thread)`. That guarantees a pool is neither in flight nor touched by another thread.

Recycle by resetting the whole pool per frame rather than freeing buffers. This returns buffers to the recordable state and lets the driver reuse backing memory, which profiles faster:

```cpp
void CommandBufferManager::reset_pools(uint32_t frameIndex) {
    for (uint32_t i = 0; i < numPoolsPerFrame; ++i)
        vkResetCommandPool(device, pools[pool_from_indices(frameIndex, i)], 0);
}
```

Size each pool for a few buffers (two primary, three secondary) so a pass that splits unexpectedly still fits.

## 19.7 Primary versus secondary buffers

Primary buffers do anything and are the only ones submitted. Their granularity is coarse: a render pass begins and ends on one thread. Secondary buffers record only draws inside one inherited pass, but many can record the same pass in parallel.

The primary starts the pass with `VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS`. Each secondary inherits pass and framebuffer, then redeclares everything else (viewport and scissor are not inherited):

```cpp
VkCommandBufferInheritanceInfo inherit{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
inherit.renderPass = pass;
inherit.framebuffer = framebuffer;
VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
begin.pInheritanceInfo = &inherit;
vkBeginCommandBuffer(secondary, &begin);
vkCmdSetViewport(secondary, 0, 1, &viewport);
vkCmdSetScissor(secondary, 0, 1, &scissor);
vkCmdBindPipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
vkCmdDrawIndexed(secondary, indexCount, 1, 0, 0, 0);
vkEndCommandBuffer(secondary);
```

The primary splices them with `vkCmdExecuteCommands`. Threads finish in any order, so tag each secondary with a sort key and execute the sorted array.

## 19.8 Recording draws across tasks

Partition meshes, dispatch one task per partition, wait, then execute in key order:

```cpp
SecondaryDrawTask tasks[kParallel]{};
uint32_t start = 0;
for (uint32_t i = 0; i < kParallel; ++i) {
    tasks[i].Init(scene, renderer, start, start + drawsPerTask, i);
    start += drawsPerTask;
    scheduler.AddTaskSetToPipe(&tasks[i]);
}
for (uint32_t i = 0; i < kParallel; ++i) {
    scheduler.WaitforTask(&tasks[i]);
    vkCmdExecuteCommands(primary, 1, &tasks[i].commandBuffer);
}
```

Group by render pass in real code, not fixed mesh counts; Chapter 20 automates that choice, and Chapter 21 extends the overlap to async compute.

## Recap

- Tasks beat one-thread-per-job; enkiTS schedules `ITaskSet` units with work stealing, plus pinned tasks for services.
- A pinned I/O thread decodes files and queues uploads without stalling the frame.
- Request graphics and transfer queues at device creation; a dedicated transfer family overlaps uploads with drawing.
- Upload via a mapped staging buffer, transfer copy, and barriers that also move queue-family ownership.
- Allocate `frames * threads` pools keyed by `(frame, thread)`; recycle with `vkResetCommandPool`.
- Primaries submit and own passes; secondaries parallelise draws inside one pass and splice via sorted `vkCmdExecuteCommands`.

## Exercises

1. **Overlap an upload with rendering.** Stream a 4K texture through the async path while rendering a spinning mesh. Confirm in a profiler that the transfer queue is busy during drawing, then force `transferQueue = graphicsQueue` with a mutex and note the frame-time delta.
2. **Split one pass across secondaries.** Record a 1,000-draw pass as one primary, then as one primary plus four secondaries cut by mesh quarters. Verify identical pixels, check each secondary sets its own viewport and scissor, and shuffle completion order to prove the sort key keeps output stable.
