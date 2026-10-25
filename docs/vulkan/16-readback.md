# Chapter 16: Getting Data Back

> **Objectives:** Reverse the transfer direction with host-visible readback buffers; make device writes visible with barriers, `VK_ACCESS_HOST_READ_BIT`, and mapped-memory invalidate; create and reset query pools and wrap work with begin/end commands; use occlusion queries for visibility and pipeline statistics for bottleneck hunting; measure GPU time with timestamps scaled by `timestampPeriod`; time readback with fences. Chapter 17 then tours a small engine: timestamp queries in GpuDevice, RenderDoc captures for frame inspection.

Most Vulkan work in this guide has been fire and forget: record, submit, and a finished image appears. Sometimes you need an answer back. How many pixels survived depth testing? How long did a pass take? What bytes are actually in an image, for a screenshot or a compute result saved to disk?

This chapter covers the two feedback paths. **Queries** collect small counters and timings while commands execute. **Host readback** copies real data into a mapped buffer the CPU can read.

## 16.1 Query pools: one type per pool

A query is a slot in a **query pool**. Each pool holds exactly one query type:

```c
VkQueryPoolCreateInfo poolInfo = { 0 };
poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
poolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
poolInfo.queryCount = 4;

VkQueryPool occlusionPool = VK_NULL_HANDLE;
vkCreateQueryPool(device, &poolInfo, NULL, &occlusionPool);
```

The three types are `VK_QUERY_TYPE_OCCLUSION` (samples passing depth and stencil tests), `VK_QUERY_TYPE_PIPELINE_STATISTICS` (pipeline counters, selected with `pipelineStatistics` flags), and `VK_QUERY_TYPE_TIMESTAMP` (device clock snapshots). Destroy the pool when done with `vkDestroyQueryPool`.

Slots start undefined. Before first use, and between reuses, reset them on the device:

```c
vkCmdResetQueryPool(cmd, occlusionPool, 0, 4);
```

Reset runs on the queue, so record it in the same command buffer ahead of the queries, or in an earlier submission the queries wait on. Resetting on the host between frames is a common mistake: the reset must be ordered against the GPU work it prepares.

## 16.2 Wrapping work with begin and end

Occlusion and statistics queries bracket the commands they measure:

```c
vkCmdResetQueryPool(cmd, occlusionPool, 0, 1);
vkCmdBeginQuery(cmd, occlusionPool, 0, 0);

/* draws to measure go here */
vkCmdDraw(cmd, vertexCount, 1, 0, 0);

vkCmdEndQuery(cmd, occlusionPool, 0);
```

`vkCmdBeginQuery` takes flags, and the only defined bit is `VK_QUERY_CONTROL_PRECISE_BIT`. Without it the device may return an approximate count, which is cheaper on some hardware. Leave it clear for a visible-or-not test where any nonzero value means visible; set it when you need an exact pixel area.

Begin and end calls must pair exactly. An unended query never becomes available, and ending a query twice or ending one without a begin is undefined behavior. Pull the result to the host with `vkGetQueryPoolResults`:

```c
uint64_t samples = 0;
VkResult res = vkGetQueryPoolResults(device, occlusionPool, 0, 1,
    sizeof(samples), &samples, sizeof(samples),
    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
```

Here `firstQuery` and `queryCount` select slots, each result lands `stride` bytes apart, and `dataSize` caps how much Vulkan writes. A `stride` smaller than one result's size yields undefined data. `VK_QUERY_RESULT_64_BIT` widens results to 64 bits. `VK_QUERY_RESULT_WAIT_BIT` blocks until the query is ready; without it the call returns `VK_NOT_READY` instead. `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` appends a ready flag per query, and `VK_QUERY_RESULT_PARTIAL_BIT` permits a provisional value before completion.

For GPU-side consumption, copy results into a buffer followed by a barrier from transfer-write to the consuming stage/access so later work can consume them without touching the host:

```c
vkCmdCopyQueryPoolResults(cmd, occlusionPool, 0, 1,
    resultBuffer, 0, sizeof(uint64_t),
    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
```

## 16.3 Occlusion queries: visibility and level of detail

An occlusion query counts fragments that pass depth and stencil testing. With those tests disabled it counts every rasterized sample, which is simply the primitive area in pixels.

The classic pattern renders scene depth first, then draws a cheap proxy box per object inside a query. Zero means hidden, so skip the real mesh. Use imprecise mode for the visibility test and reserve precise mode for area-based detail selection. Small nonzero means distant, so pick a low-detail variant.

## 16.4 Pipeline statistics: counting what the pipeline did

A statistics pool enables a subset of counters at creation:

```c
VkQueryPoolCreateInfo statsInfo = { 0 };
statsInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
statsInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
statsInfo.queryCount = 1;
statsInfo.pipelineStatistics =
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;

VkQueryPool statsPool = VK_NULL_HANDLE;
vkCreateQueryPool(device, &statsInfo, NULL, &statsPool);
```

Available counters include input-assembly vertices and primitives, vertex, geometry, tessellation, fragment, and compute invocations, plus clipper inputs and outputs. Results arrive as consecutive integers in increasing flag-bit order, one per enabled bit. Enable only what you need, because counters can cost performance on some implementations.

Treat these numbers as relative. Cache reuse, helper invocations, and clipper behavior vary by device. Compare two runs on the same GPU; do not compare counts across vendors.

## 16.5 Timestamps: measuring GPU time

Timestamp queries do not bracket work. They stamp the clock at a pipeline stage:

```c
vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
    timestampPool, 0);
/* work to time */
vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
    timestampPool, 1);
```

Take two stamps, subtract for ticks, then scale by the device period:

```c
uint64_t ticks[2] = { 0, 0 };
vkGetQueryPoolResults(device, timestampPool, 0, 2,
    sizeof(ticks), ticks, sizeof(uint64_t),
    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

float period = physProps.limits.timestampPeriod;
double elapsedNs = (double)(ticks[1] - ticks[0]) * period;
```

`timestampPeriod` comes from `vkGetPhysicalDeviceProperties` and reports nanoseconds per tick. Ticks always increase, but the unit varies by device, so the multiply is never optional.

`VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT` before the work and `VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT` after is the standard idiom for timing a full interval. Use the same stage for both stamps when narrowing to one region for a clean delta. Not every stage supports stamping on every device; an unsupported request is honored at the next supported stage, which silently shrinks the interval. When deltas look suspiciously small, suspect stage fallback before suspecting the workload.

> **Misconception:** "A timestamp value is nanoseconds, and a mapped readback is ready once submit returns."
>
> Neither is true. A timestamp is device ticks; multiply the delta by `timestampPeriod` to reach nanoseconds. And submit-return only means the driver accepted the batch. The copy may not have run, caches may not be visible to the host, and non-coherent mappings may still hold stale cache lines. Wait on a fence, apply the host barrier, invalidate if non-coherent, and only then read. Skipping any step reads whatever happened to be in the cache, which sometimes looks correct right up until it does not.

## 16.6 Reading real data: image to buffer to host

To pull pixels or compute output back, copy into a buffer backed by host-visible memory, then read through the mapping. The destination needs `VK_BUFFER_USAGE_TRANSFER_DST_BIT` plus `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`. Keep hot render targets device-local and reserve host-visible memory for the small readback buffer.

Transition the image to `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`, then copy:

```c
VkBufferImageCopy region = { 0 };
region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
region.imageSubresource.layerCount = 1;
region.imageExtent.width = width;
region.imageExtent.height = height;
region.imageExtent.depth = 1;

vkCmdCopyImageToBuffer(cmd, srcImage,
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    readbackBuffer, 1, &region);
```

Then publish the write to the host with a buffer barrier:

```c
VkBufferMemoryBarrier toHost = { 0 };
toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
toHost.buffer = readbackBuffer;
toHost.size = VK_WHOLE_SIZE;

vkCmdPipelineBarrier(cmd,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0, 0, NULL, 1, &toHost, 0, NULL);
```

The barrier moves ownership from `VK_ACCESS_TRANSFER_WRITE_BIT` to `VK_ACCESS_HOST_READ_BIT`. On the host, wait on the submission fence, then handle cache coherence:

```c
vkQueueSubmit(graphicsQueue, 1, &submit, readbackFence);
vkWaitForFences(device, 1, &readbackFence, VK_TRUE, UINT64_MAX);

if ((memProps.memoryTypes[typeIndex].propertyFlags &
     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
    VkMappedMemoryRange range = { 0 };
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = readbackMemory;
    range.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(device, 1, &range);
}

/* now safe: memcpy or parse from the mapped pointer */
```

Coherent mappings skip the invalidate but still need the fence plus the barrier. Non-coherent mappings need all three: the fence for execution, the barrier for visibility, the invalidate for the CPU cache. Treat a submitted range as lent to the device until its fence signals.

> **Flint note:** Flint almost never reads back per frame. Queries stay off in normal builds; GPU timing comes from RenderDoc, Radeon GPU Profiler, or Nsight Graphics. Host readback exists for diagnostics only: screenshots and validation dumps. Gate it behind a flag and keep it out of the hot loop, because the fence wait stalls everything behind it.

## Recap

- Query pools hold one query type each; reset slots on the device before each use and pair every begin with an end.
- `vkGetQueryPoolResults` pulls counters to the host with 64-bit, wait, availability, and partial flags; `vkCmdCopyQueryPoolResults` keeps them on the GPU followed by a barrier from transfer-write to the consuming stage/access.
- Occlusion queries test visibility and screen area; imprecise mode answers zero versus nonzero, precise mode costs more.
- Statistics queries count pipeline events in flag-bit order; enable few counters and compare runs on the same device.
- Timestamps are ticks stamped at a pipeline stage; top-of-pipe before and bottom-of-pipe after is the standard full-interval idiom, and a same-stage pair narrows to one region; scale the delta by `timestampPeriod`.
- Host readback copies the image to a host-visible buffer, barriers transfer-write to host-read, waits on a fence, and invalidates non-coherent mappings before the CPU reads.

## Exercises

1. **Timestamp a draw pair.** Create a two-slot timestamp pool, stamp top-of-pipe before and bottom-of-pipe after a single draw, and report elapsed nanoseconds using `timestampPeriod`. Then move the second stamp to an unsupported mid-geometry stage and state whether the delta shrinks, and why stage fallback explains it.
2. **Screenshot readback.** Render a solid color, copy a 64x64 region with `vkCmdCopyImageToBuffer` into a host-visible buffer, barrier to `VK_ACCESS_HOST_READ_BIT`, wait on a fence, and invalidate if non-coherent. Verify the bytes on the host, then remove the invalidate on a non-coherent type and describe which stale pattern appears and which of the three steps (fence, barrier, invalidate) you broke.
