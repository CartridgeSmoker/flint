# Chapter 1: What Vulkan Is

> **Objectives:**
> - Explain what Vulkan is and why it trades OpenGL convenience for explicit control.
> - Describe the instance, physical device, logical device, and queue hierarchy.
> - Distinguish layers from extensions and say when each is enabled.
> - Trace a frame from loader to presentation and list what you will set up before coding.
> - Create an instance and enumerate physical devices and their queue families.

OpenGL spent twenty years trying to be helpful. It tracked hidden state, guessed where memory should live, recompiled shaders unprompted, and funneled multithreaded work through one global lock. On old hardware that kindness was welcome. On a modern machine with many CPU cores feeding a hungry GPU, it is a bottleneck you can measure.

Vulkan, shipped by the Khronos Group in 2016 from AMD's donated Mantle design, starts from the opposite idea: the driver stays thin, and your program owns lifetime, memory, synchronization, and error checking. It is also the portable member of its generation: Direct3D 12 stays on Windows and Metal on Apple platforms, while Vulkan covers Windows, Linux, Android, and others with one API.

Vulkan is more than graphics. Work splits into transfer (copies), compute (shaders over data), and graphics (points, lines, triangles, blending). A device may support only a subset, so even putting a picture on screen, called presentation, lives outside core as an extension.

## 1. What Vulkan is not

If OpenGL was a shopkeeper who stocked shelves and caught your errors, Vulkan is a warehouse you run yourself. Nothing happens unless you request, allocate, and synchronize it. Every difference below flows from moving control out of the driver and into your program.

**Thinner driver, freer CPU.** OpenGL infers intent through a large implicit state machine. Vulkan keeps almost none, so each call costs less once your program is correct.

**Threading that scales.** OpenGL serializes work around shared state. Vulkan lets each thread record its own command buffers into its own pool, merging them only at submit.

**Shaders compiled ahead of time.** OpenGL compiles GLSL at runtime, so hitches land mid-frame. Vulkan consumes SPIR-V you produce offline with the SDK's `glslangValidator`.

**Memory you choose.** OpenGL places resources by hidden heuristics. Vulkan advertises heaps - device-local, host-visible, and combinations - and you bind each resource yourself, typically staging on the host then transferring to device-local memory.

**Predictable submission and optional validation.** Vulkan runs buffers in submission order, with semaphores ordering work across queues, barriers and events ordering work within a queue, and fences synchronizing the host with the device. Checks live in layers you enable for development, not in the driver hot path.

| OpenGL habit | Vulkan replacement |
| --- | --- |
| Driver tracks state and resources | You create and destroy every object explicitly |
| GLSL compiled at draw time | SPIR-V compiled offline, loaded upfront |
| Driver picks memory placement | You query heaps and bind each resource yourself |
| One implicit context, poor threading | Many command buffers recorded in parallel |
| Always-on error checking | Validation layers, on in development, off in release |

Concretely, there is no hidden state, no implicit memory or synchronization, and no always-on checking. Misuse produces corruption rather than friendly errors, so this guide keeps validation on early.

> **Misconception:** "Explicit means automatically faster."
>
> No. Explicit means *predictable*; speed is what you earn by using that control well. A program that builds a pipeline per draw or re-records every buffer each frame will lose to OpenGL. The wins come from doing costly work once - pipelines, layouts, buffers - then reusing them.

## 2. Instances, physical devices, and logical devices

Vulkan organizes itself as a short hierarchy: instance, physical device, logical device, queue.

The **instance** (`VkInstance`) is the root and your handshake with Vulkan. It holds program state and optionally enables validation layers and instance extensions such as WSI. Create one first; almost everything else is discovered through it.

A **physical device** (`VkPhysicalDevice`) is hardware as seen by the system. There is a fixed set per system; you enumerate rather than create them. Each reports properties, features, memory layout, and queue families.

A **logical device** (`VkDevice`) is your reservation on one physical device. You choose queues, features, and device extensions, then call `vkCreateDevice`. Most programs create one instance, one physical device pick, and one logical device. Nearly every later call takes that device.

**Queues** are the channels to hardware where work executes. Each physical device exposes queue families with capabilities such as graphics, compute, transfer, or sparse binding. You request queues at device creation and submit command buffers to them later. Order holds within one queue; across queues work runs in parallel unless you synchronize it.

Objects come in two flavors. Dispatchable objects (instance, physical device, device, queue, command buffer) carry a dispatch table for layers and drivers; nondispatchable objects (semaphore, image, buffer, and most others) are plain handles under a dispatchable parent. Names guide you: `vkCreate*` takes a `Vk*CreateInfo`, `vkDestroy*` releases it, `vkGet*` queries properties, and `vkCmd*` records into a command buffer.

> **Flint note:** Flint is a Vulkan client on Wayland, so it creates its instance with `VK_KHR_surface` plus `VK_KHR_wayland_surface` and renders into swapchain images the compositor shows on vsync. Expect the paired shape too: every `vkCreate*` with its `vkDestroy*`, every `CreateInfo` with its `sType` set.

## 3. Creating an instance and finding devices

Fill a parameter structure, point at it, and call create. This pattern repeats through the whole guide.

```c
VkApplicationInfo appInfo = { 0 };
appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
appInfo.pApplicationName = "flint-probe";
appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
appInfo.apiVersion = VK_API_VERSION_1_0;

VkInstanceCreateInfo createInfo = { 0 };
createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
createInfo.pApplicationInfo = &appInfo;

VkInstance instance = VK_NULL_HANDLE;
VkResult result = vkCreateInstance(&createInfo, NULL, &instance);
if (result != VK_SUCCESS) {
    // Handle failure: no Vulkan, no loader, or incompatible version.
}
```

`sType` identifies the structure for validation. `pNext` heads an extension chain and stays `NULL` unless an extension says otherwise. The allocator stays `NULL`; for custom `VkAllocationCallbacks` see Chapter 4 section 1. Set `apiVersion` to the minimum you need.

Once the instance exists, enumerate physical devices with the standard count-then-fetch pattern.

```c
uint32_t deviceCount = 0;
result = vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);
if (result != VK_SUCCESS) {
    // Handle failure: no Vulkan, no loader, or incompatible version.
}
if (deviceCount == 0) {
    return VK_ERROR_INITIALIZATION_FAILED; // no Vulkan-capable hardware
}

VkPhysicalDevice devices[8];
uint32_t fetchCount = deviceCount > 8 ? 8 : deviceCount;
result = vkEnumeratePhysicalDevices(instance, &fetchCount, devices);
if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    // Handle failure: enumeration itself failed.
}
if (result == VK_INCOMPLETE) {
    // fetchCount holds what was actually written; devices[8] truncates the list.
    // See Chapter 2 section 2 for the canonical VK_INCOMPLETE-retry loop.
}

for (uint32_t i = 0; i < fetchCount; i++) {
    VkPhysicalDeviceProperties props = { 0 };
    vkGetPhysicalDeviceProperties(devices[i], &props);
    // props.deviceName, props.apiVersion, props.deviceType, props.limits
}
```

Each handle answers queries for properties, features, memory, and queue families. Run that dump on your machine: you may only enable features the device advertises. Logical-device creation then selects from what you found, with one `VkDeviceQueueCreateInfo` per family plus needed features and device extensions. Enable only what you will use, with one priority float per queue as a scheduling hint.

Creation flows inward and destruction flows outward in reverse: wait for idle, destroy device, then instance.

```c
vkDeviceWaitIdle(device);
vkDestroyDevice(device, NULL);
vkDestroyInstance(instance, NULL);
```

## 4. Layers and extensions

Two mechanisms extend the core, and they solve different problems.

**Layers** intercept calls to add validation, logging, or profiling. Enable them at instance creation and list them with `vkEnumerateInstanceLayerProperties`. The standard validation layer belongs in every debug build and in no release build: `VK_LAYER_KHRONOS_validation`, falling back to `VK_LAYER_LUNARG_standard_validation` only where the new name is absent.

**Extensions** add functionality. Instance extensions affect the whole system; device extensions light up per-device features. Presentation is the canonical example: `VK_KHR_surface` plus `VK_KHR_wayland_surface` on the instance and `VK_KHR_swapchain` on the device. Query them with `vkEnumerateInstanceExtensionProperties` and `vkEnumerateDeviceExtensionProperties`, and load new entry points with `vkGetInstanceProcAddr` or `vkGetDeviceProcAddr`.

Per-device layers are deprecated; enable layers only on the instance (see Chapter 2 section 2).

## 5. How a frame flows

1. **Loader handshake.** The loader finds the driver, loads entry points, and injects your layers.
2. **Instance and device.** Create the instance, pick a physical device, query queues, heaps, and extensions, then create the logical device. Chapter 2 builds instance/physical/logical/queue reservation; Chapter 4 turns the memory query into allocation and binding.
3. **Window and swapchain.** Through WSI, create a surface and a swapchain whose images rotate between painting and display.
4. **Resources and pipeline.** Create buffers and images, bind memory, load SPIR-V modules, and build pipelines. Pipelines bake shaders and fixed state into one reusable handle; build once and reuse through a pipeline cache.
5. **Record.** From a command pool, record commands: open a render pass, bind pipeline and descriptors, draw, close the pass. Buffers for repeating work are recorded once and resubmitted.
6. **Submit and present.** Acquire a swapchain image, submit with semaphores and fences, then present.

Steps 2-4 run rarely. Steps 5-6 are the per-frame loop, and the game is keeping step 5 cheap through reuse and parallelism. Chapter 3 turns on validation and debugging so each of these steps fails loudly while you are learning it.

## 6. Setting up before you code

Install in order: **GPU driver with Vulkan support** first, then **Python** and **CMake** on your `PATH`, then the **LunarG SDK** - loader, validation layers, SPIR-V tools, headers, docs, and samples. Verify by running a sample from the SDK's `Bin` directory (`Bin32` on 32-bit systems). If its window opens, driver and SDK agree and you are ready.

## Recap

- Vulkan trades OpenGL's implicit driver for an explicit contract: your program owns lifetime, memory, submission, and synchronization.
- One instance owns the session. Physical devices are enumerated; logical devices reserve queues, features, and extensions.
- Layers add development behavior. Extensions add shipping behavior. Both are opt-in.
- Resources need explicit backing. Shaders ship as SPIR-V baked into reusable pipelines.
- Presentation is an extension, so a Wayland client like Flint enables surface and swapchain extensions.
- Setup means driver, Python, CMake, then the LunarG SDK, confirmed by a running sample.

## Exercises

1. **Inspect your machine.** Run `vulkaninfo --summary` and note device count, queue families, and one heap with its flags. Which heap suits a vertex buffer the GPU reads every frame, and why? Then extend the snippet into a program printing `deviceName`, `apiVersion`, `deviceType`, and queue flags.
2. **Turn on validation.** Enable `VK_LAYER_KHRONOS_validation` (falling back only if needed), then pass a bad feature or skip the count query. What message appears, at which call? Disable layers and confirm it disappears.
