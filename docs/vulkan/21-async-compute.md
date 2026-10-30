# Chapter 21: Async Compute and Timelines

> **Objectives:** Replace per-frame fences with one timeline semaphore; signal and wait on it from CPU and GPU; submit to a separate async compute queue with correct cross-queue synchronisation; size and dispatch workgroups; move cloth simulation onto the async queue. Chapter 15 introduces the timeline and barrier primitives reused here, and Chapter 22 carries the async-queue pattern into GPU-driven submission.

> **Flint note:** Flint handles simulation work the way this guide handles cloth: keep data on the device, advance it on an async queue, hand graphics only a timeline value to wait on. No staging copies, no extra fences.

## 21.1 One counter instead of many fences

A frames-in-flight renderer accumulates synchronisation objects: one fence per frame, one semaphore to gate present, more of each for every extra submission. A second queue doubles the pile again.

A timeline semaphore collapses it into one increasing 64-bit counter. Signal value N when submission N completes; wait for value M wherever M must have finished, on CPU or GPU. Two rules: signal values must strictly increase, and a submission must never wait for a value only it signals. The validation layers catch both.

## 21.2 Enabling and creating a timeline semaphore

Timeline semaphores are core in Vulkan 1.2. Target 1.2+ and there is no extension; on older targets enable `VK_KHR_timeline_semaphore`. Opt in through a feature chain at device creation, after querying support:

```cpp
VkPhysicalDeviceTimelineSemaphoreFeatures queried{
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };

VkPhysicalDeviceFeatures2 query{
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
query.pNext = &queried;
vkGetPhysicalDeviceFeatures2(physicalDevice, &query);
// Fail device creation here if queried.timelineSemaphore == VK_FALSE.

VkPhysicalDeviceTimelineSemaphoreFeatures enabled{
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
enabled.timelineSemaphore = VK_TRUE;

VkPhysicalDeviceFeatures2 enabledFeatures{
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
enabledFeatures.pNext = &enabled;

VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
deviceInfo.pNext = &enabledFeatures;
// Queue infos and extension list as in Chapter 2.
deviceInfo.queueCreateInfoCount = queueCount;
deviceInfo.pQueueCreateInfos = queues;
vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);
```

Creation adds one struct to the familiar path:

```cpp
VkSemaphoreTypeCreateInfo typeInfo{
    VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
typeInfo.initialValue = 0;

VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
semInfo.pNext = &typeInfo;
vkCreateSemaphore(device, &semInfo, nullptr, &frameTimeline);
```

Omit the `pNext` chain and you get a classic binary semaphore. The chain is the whole difference.

## 21.3 Waiting on the CPU, signalling on the GPU

The CPU waits by naming a semaphore and a value, blocking until the counter reaches it or the timeout expires:

```cpp
VkSemaphoreWaitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
waitInfo.semaphoreCount = 1;
waitInfo.pSemaphores = &frameTimeline;
waitInfo.pValues = &completedFrame;

vkWaitSemaphores(device, &waitInfo, UINT64_MAX);
```

One call can wait on several timelines: the default waits for all, `VK_SEMAPHORE_WAIT_ANY_BIT` returns on the first. Use a finite timeout in debug builds so a signalling bug surfaces as `VK_TIMEOUT`. `vkGetSemaphoreCounterValue` polls; `vkSignalSemaphore` signals from the host.

On the GPU the same object travels with the submission. This guide uses the `synchronization2` form (`VkSubmitInfo2`, core in 1.3); the older `VkSubmitInfo` plus `VkTimelineSemaphoreSubmitInfo` chain is equivalent. A submission that must not overrun the frames in flight waits for an old value and signals the new one (binary acquire/present semaphores mix into the same arrays; their `value` is ignored):

```cpp
bool needWait = absoluteFrame >= kMaxFrames - 1;

VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
if (needWait) {
    wait.semaphore = frameTimeline;
    wait.value = absoluteFrame - (kMaxFrames - 1);
    wait.stageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
}

VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
signal.semaphore = frameTimeline;
signal.value = absoluteFrame + 1;
signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

VkCommandBufferSubmitInfo cmdInfo{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
cmdInfo.commandBuffer = cmd;

VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
submit.waitSemaphoreInfoCount = needWait ? 1 : 0;
submit.pWaitSemaphoreInfos = &wait;
submit.commandBufferInfoCount = 1;
submit.pCommandBufferInfos = &cmdInfo;
submit.signalSemaphoreInfoCount = 1;
submit.pSignalSemaphoreInfos = &signal;
vkQueueSubmit2(graphicsQueue, 1, &submit, VK_NULL_HANDLE);
```

No fence is needed: the CPU waits with `vkWaitSemaphores` on the same counter the GPU signals.

> **Misconception: "The first submission should wait on zero."** A fresh timeline starts at zero already signalled, so waiting for zero is redundant, and waiting for one before anything signalled deadlocks. Submit the first frame with no timeline wait, then use strictly increasing values.

## 21.4 A separate async compute queue

Graphics and compute stages share one pool of execution units. A graphics-heavy frame leaves units idle during fixed-function work and between passes; an independent compute queue lets the scheduler fill those gaps.

Prefer a compute-only family (`VK_QUEUE_COMPUTE_BIT` without graphics); fall back to a second queue from a combined family. Keep a distinct `VkQueue` and record compute work into its own command buffers, since one command buffer cannot serve two queues.

Give the compute stream its own timeline, advancing it per submission and skipping the wait on the first:

```cpp
bool hasWait = lastComputeValue > 0;

VkSemaphoreSubmitInfo cWait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
cWait.semaphore = computeTimeline;
cWait.value = lastComputeValue;
cWait.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

VkSemaphoreSubmitInfo cSignal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
cSignal.semaphore = computeTimeline;
cSignal.value = lastComputeValue + 1;
cSignal.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
// submit with wait count (hasWait ? 1 : 0), signal count 1, then:
lastComputeValue += 1;
```

Cross-queue ordering is one extra wait on the consumer. If graphics consumes positions the compute shader wrote, wait for the compute timeline at the consuming stage:

```cpp
VkSemaphoreSubmitInfo gfxWait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
gfxWait.semaphore = computeTimeline;
gfxWait.value = lastComputeValue;
gfxWait.stageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
```

Choose the stage honestly: vertex input for displaced positions, fragment-shader stage for a filled texture. Too broad stalls the pipeline; too narrow is a data race. Create shared buffers with `VK_SHARING_MODE_CONCURRENT` (or keep `EXCLUSIVE` with an ownership transfer as in Chapter 15), and add the wait only on frames that dispatched compute work.

## 21.5 Compute shaders in brief

Every dispatch runs the same entry point across many threads on different data. The local size sets the workgroup shape; `vkCmdDispatch(x, y, z)` sets how many workgroups launch:

```glsl
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
```

```cpp
uint32_t groups = (vertexCount + 63u) / 64u;
vkCmdDispatch(cmd, groups, 1, 1);
```

Inside the shader, `gl_GlobalInvocationID` is each thread's unique index; threads past the end return early. For a 2D image, divide width and height by the local sizes so each texel is covered once. Threads in one workgroup can meet at `barrier()`; threads in different workgroups synchronise only by ending the dispatch.

## 21.6 Cloth on the async queue

Cloth fits because the CPU version pays twice: cycles integrating springs, then a full copy of fresh positions, normals, and tangents to the GPU. Integrating on the device removes the copy.

Switch the buffers to device-local storage uploaded once. They double as vertex buffers, so include both usages and share them across both families:

```cpp
VkBufferCreateInfo bufInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
bufInfo.size = positionBytes;
bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
              | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
              | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
bufInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
bufInfo.queueFamilyIndexCount = 2;
bufInfo.pQueueFamilyIndices = queueFamilies; // graphics + compute
vkCreateBuffer(device, &bufInfo, nullptr, &positionBuffer);
```

Expose the data as one storage block per array, since each block holds one runtime array, and mark never-written inputs `readonly`:

```glsl
layout(set = 0, binding = 1) buffer PhysicsMesh {
    uint indexCount; uint vertexCount; PhysicsVertex vertices[];
};
layout(set = 0, binding = 2) buffer PositionData { float positions[]; };
layout(set = 0, binding = 3) buffer NormalData { float normals[]; };
layout(set = 0, binding = 4) readonly buffer IndexData { uint indices[]; };
```

Use flat `float` arrays rather than `vec3` arrays: `std140` pads each `vec3` to 16 bytes, disagreeing with packed CPU vertices. Separate arrays also let the depth pass bind positions alone.

The shader mirrors the CPU loop, with one GLSL adjustment: no references, so every access reads and writes the buffer directly:

```glsl
uint v = gl_GlobalInvocationID.x;
if (v >= vertexCount) return;

vec3 force = gravity * mass;
for (uint j = 0; j < vertices[v].jointCount; ++j)
    force += springPull(vertices[v], j);
force += vertices[v].velocity * (-damping);
vertices[v].force = force;

vec3 current = vertices[v].position;
vec3 next = integrate(current, vertices[v].previousPosition, force, dt);
vertices[v].previousPosition = current;
vertices[v].position = next;
vertices[v].velocity = next - current;
positions[v * 3 + 0] = next.x;
positions[v * 3 + 1] = next.y;
positions[v * 3 + 2] = next.z;
```

Forces must be computed for all vertices before any position integrates: one thread per vertex with a force dispatch and an integrate dispatch separated by a compute barrier (best for large cloths), or one thread per mesh looping internally (best for many small cloths).

## Recap

- One timeline replaces the fence-per-frame set: waits and signals are values on one counter, usable from CPU and GPU.
- Enable the feature, chain `VkSemaphoreTypeCreateInfo` at creation, signal ever-increasing values, never wait for a value only the same submit signals.
- The host uses `vkWaitSemaphores` / counter query / host signal; the device uses `VkSemaphoreSubmitInfo` in `VkSubmitInfo2`, mixing binary and timeline entries.
- The async queue needs its own command buffers, its own timeline, and one wait on the consuming graphics submission at the honest stage.
- Compute launches are local size times dispatch count indexed by `gl_GlobalInvocationID`; cloth becomes device-local storage with flat float arrays and direct buffer writes.

## Exercises

1. **Collapse three fences into one timeline.** Convert a double- or triple-buffered loop from one fence per frame to a single timeline. Wait on the CPU with `vkWaitSemaphores` before reusing frame resources, wait on the GPU at `TOP_OF_PIPE`, signal the new value at submit. Confirm the counter rises every frame and the first frame submits with no wait.
2. **Move one update to the async queue.** Port a small CPU vertex animation to a compute shader on the compute queue: device-local buffer, one dispatch per frame, graphics waiting on the compute timeline at vertex-input stage. Disable the wait to confirm corruption returns, re-enable it, then profile with and without overlap.
