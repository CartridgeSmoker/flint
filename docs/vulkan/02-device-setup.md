# Chapter 2: Instance, Device, and the First Program

> **Objectives:**
> - Recite the canonical setup order: instance, layers/extensions, physical device, queue families, logical device, queues.
> - Enumerate layers and extensions with the two-call idiom and enable only what exists.
> - Create a `VkInstance` with `VkApplicationInfo` and pick a `VkPhysicalDevice` deliberately.
> - Find a graphics queue family and create a `VkDevice` with usable `VkQueue` handles.
> - Map every later stage of the first program to the setup it depends on.

A minimal triangle touches a dozen object types before a single pixel appears. This chapter gives you the map first, then makes the first stops real: loader, hardware, logical handle, and queues. Nothing draws yet, but everything later hangs off what you build here.

```c
/* 1. Handshake with the loader */
vkEnumerateInstanceLayerProperties(&layerCount, NULL);
vkEnumerateInstanceExtensionProperties(NULL, &extCount, NULL);
vkCreateInstance(&instanceInfo, NULL, &instance);      /* VkInstance */

/* 2. Pick the hardware */
vkEnumeratePhysicalDevices(instance, &gpuCount, gpus); /* VkPhysicalDevice */
vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueCount, queues);
vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);
vkEnumerateDeviceExtensionProperties(gpu, NULL, &devExtCount, NULL);

/* 3. Open the logical device and its queues */
vkCreateDevice(gpu, &deviceInfo, NULL, &device);       /* VkDevice */
vkGetDeviceQueue(device, graphicsFamily, 0, &queue);

/* 4. Presentation preview (full detail in Chapter 7) */
vkCreateWaylandSurfaceKHR(instance, &surfaceInfo, NULL, &surface);
vkGetPhysicalDeviceSurfaceSupportKHR(gpu, queueFamily, surface, &supported);
vkCreateSwapchainKHR(device, &swapchainInfo, NULL, &swapchain);
vkGetSwapchainImagesKHR(device, swapchain, &imgCount, images);

/* 5. Work preview: pools, resources, shaders, pipeline, frame loop */
vkCreateCommandPool(device, &poolInfo, NULL, &cmdPool);
vkAllocateCommandBuffers(device, &allocInfo, &cmd);
vkCreateImage(device, &imageInfo, NULL, &depthImage);  /* + allocate, bind, view */
vkCreateShaderModule(device, &vertInfo, NULL, &vertModule);
vkCreateRenderPass(device, &rpInfo, NULL, &renderPass);
vkCreateGraphicsPipelines(device, cache, 1, &gfxInfo, NULL, &pipeline);
vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                      imageAcquired, VK_NULL_HANDLE, &imageIndex);
vkQueueSubmit(queue, 1, &submitInfo, drawFence);
vkQueuePresentKHR(queue, &presentInfo);
```

Memorize this shape. Steps 1 to 3 are this chapter. Steps 4 and 5 are previews so you can see where setup leads; later chapters fill in every stub. Each handle is minted from the previous one: no instance, no device list; no queue-family query, no valid device request; no device, no queues, pools, or pipelines.

You need the SDK, a compiler, and CMake. Check with `vulkaninfo`; if it runs, driver and loader agree. Link with the CMake Vulkan module as `Vulkan::Vulkan`. All examples in this guide use C, check every `VkResult`, and clean up in reverse order. Vulkan never cleans up after you.

## 1. Instance: joining the loader

The instance (`VkInstance`) is your connection to the loader. It records the API version you want plus the layers and global extensions you enabled. Nothing hardware-specific happens here.

In code you enumerate first and create second; logically both belong to this step, because the names you discovered become the `ppEnabledLayerNames` and `ppEnabledExtensionNames` you lock in at creation. Fill `VkApplicationInfo` first:

```c
VkApplicationInfo appInfo = {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pNext = NULL,
    .pApplicationName = "Handshake",
    .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
    .pEngineName = "NoEngine",
    .engineVersion = VK_MAKE_VERSION(1, 0, 0),
    .apiVersion = VK_API_VERSION_1_0,
};
```

Set `apiVersion` to the minimum you need, not just your headers. If the driver cannot satisfy it, creation returns `VK_ERROR_INCOMPATIBLE_DRIVER`. When comparing versions, compare major and minor only, for example with `VK_VERSION_MAJOR` and `VK_VERSION_MINOR`; patch differences never decide compatibility.

Then fill `VkInstanceCreateInfo` with the verified arrays and create:

```c
const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME };

VkInstanceCreateInfo createInfo = {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pNext = NULL,
    .flags = 0,
    .pApplicationInfo = &appInfo,
    .enabledLayerCount = 1,
    .ppEnabledLayerNames = layers,
    .enabledExtensionCount = 2,
    .ppEnabledExtensionNames = exts,
};

VkInstance instance = VK_NULL_HANDLE;
VkResult res = vkCreateInstance(&createInfo, NULL, &instance);
if (res != VK_SUCCESS) {
    fprintf(stderr, "vkCreateInstance failed: %d\n", res);
    return 1;
}
/* ... use instance ... */
vkDestroyInstance(instance, NULL);
```

On Linux with Wayland you need `VK_KHR_surface` plus `VK_KHR_wayland_surface`. Calling `vkCreateWaylandSurfaceKHR` additionally requires defining `VK_USE_PLATFORM_WAYLAND_KHR` before including `vulkan/vulkan.h`. Passing `NULL` as the allocator selects the default host allocator. The `sType` identifies the structure so layers and drivers can validate it, and `pNext` heads an extension chain and stays `NULL` unless an extension says otherwise.

> **Flint note:** Flint presents through Wayland, so its instance always enables the two surface extensions above, plus `VK_EXT_debug_utils` in debug builds. A draft enabling a Windows surface extension on Linux was copied from a Windows-first source; send it back.

## 2. Layers and extensions: query before you enable

Layers intercept API calls for validation and tracing. Extensions add behavior beyond core, such as `VK_KHR_surface` for presentation. Both share one rule: enumerate first, enable second. Requesting an unadvertised name fails with `VK_ERROR_LAYER_NOT_PRESENT` or `VK_ERROR_EXTENSION_NOT_PRESENT`.

Enumeration follows a two-call idiom. Call once with `NULL` to get the count, allocate, then call again to fill. Repeat on `VK_INCOMPLETE`, which means the list grew between calls:

```c
static void list_instance_layers(void)
{
    uint32_t count = 0;
    VkResult res;
    do {
        res = vkEnumerateInstanceLayerProperties(&count, NULL);
        if (res != VK_SUCCESS || count == 0) {
            printf("no instance layers\n");
            return;
        }

        VkLayerProperties layers[count];
        res = vkEnumerateInstanceLayerProperties(&count, layers);
        if (res == VK_INCOMPLETE)
            continue;
        if (res != VK_SUCCESS)
            return;

        for (uint32_t i = 0; i < count; i++)
            printf("layer: %s -- %s\n", layers[i].layerName,
                layers[i].description);
        break;
    } while (1);
}
```

Global extensions use the same loop with `vkEnumerateInstanceExtensionProperties`, and device extensions use `vkEnumerateDeviceExtensionProperties` once you hold a physical device handle. Pass a layer name as `pLayerName` to list only that layer's extensions, or `NULL` for the global set. Instance extensions need no device; device extensions need one. Production code keeps the `VK_INCOMPLETE` retry shown here; later fragments omit it for brevity.

For development, enable `VK_LAYER_KHRONOS_validation`. Omit it in release builds to avoid overhead.

> **Misconception:** "Listing a layer turns it on, and device layers still matter."
>
> Neither is true. Enumeration only reports. Nothing takes effect until the name appears in `ppEnabledLayerNames` or `ppEnabledExtensionNames` at create time. And per-device layers are deprecated: modern Vulkan enables layers only on the instance. Setting `enabledLayerCount` in `VkDeviceCreateInfo` is a backward-compatibility stub; leave it zero.

## 3. Picking the physical device

A physical device (`VkPhysicalDevice`) is a handle to one GPU. Enumeration returns handles; it allocates nothing. List, inspect, and choose:

```c
uint32_t gpuCount = 0;
vkEnumeratePhysicalDevices(instance, &gpuCount, NULL);
if (gpuCount == 0) {
    fprintf(stderr, "no Vulkan-capable GPUs\n");
    return 1;
}

VkPhysicalDevice gpus[gpuCount];
vkEnumeratePhysicalDevices(instance, &gpuCount, gpus);
```

Do not take `gpus[0]` blindly. Read properties and prefer discrete hardware:

```c
VkPhysicalDevice chosen = gpus[0];
for (uint32_t i = 0; i < gpuCount; i++) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpus[i], &props);
    printf("gpu %u: %s (%s)\n", i, props.deviceName,
        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
        ? "discrete" : "other");
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        chosen = gpus[i];
}
```

Also query memory with `vkGetPhysicalDeviceMemoryProperties` and features with `vkGetPhysicalDeviceFeatures`. These answers decide later allocation and feature requests, and Chapter 4 builds directly on the memory answer. Confirm each planned device extension with `vkEnumerateDeviceExtensionProperties`, such as `VK_KHR_swapchain`. Physical devices are owned by the instance and need no destroy call.

## 4. Queue families: finding submittable work

Queues are grouped into families sharing capabilities flagged by `VK_QUEUE_GRAPHICS_BIT`, `VK_QUEUE_COMPUTE_BIT`, `VK_QUEUE_TRANSFER_BIT`, and `VK_QUEUE_SPARSE_BINDING_BIT`, plus a count of queues per family. Each entry also reports `timestampValidBits` for timestamp precision and `minImageTransferGranularity` for the smallest transfer unit. One family often covers several flags.

Query with `vkGetPhysicalDeviceQueueFamilyProperties`. It returns `void`, so there is no status to check:

```c
uint32_t familyCount = 0;
vkGetPhysicalDeviceQueueFamilyProperties(chosen, &familyCount, NULL);

VkQueueFamilyProperties families[familyCount];
vkGetPhysicalDeviceQueueFamilyProperties(chosen, &familyCount, families);

uint32_t graphicsFamily = UINT32_MAX;
for (uint32_t i = 0; i < familyCount; i++) {
    if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        graphicsFamily = i;
        break;
    }
}
if (graphicsFamily == UINT32_MAX) {
    fprintf(stderr, "no graphics queue family\n");
    return 1;
}
```

You need only the family index for now. Presentation support (`vkGetPhysicalDeviceSurfaceSupportKHR`) refines the choice once a `vkCreateWaylandSurfaceKHR` surface exists; Chapter 7 revisits this and keeps the index that both draws and presents.

## 5. Creating the logical device and its queues

The logical device (`VkDevice`) is your working view of one physical device. Creating it also creates the queues you request: describe one `VkDeviceQueueCreateInfo` per wanted family, then wrap them in `VkDeviceCreateInfo`. The `pQueuePriorities` array holds relative scheduling hints from 0.0 to 1.0 and must stay alive through `vkCreateDevice`. The number of distinct priority levels you may use is bounded by `discreteQueuePriorities` in `VkPhysicalDeviceLimits`:

```c
float priority = 1.0f;
VkDeviceQueueCreateInfo queueInfo = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .pNext = NULL,
    .flags = 0,
    .queueFamilyIndex = graphicsFamily,
    .queueCount = 1,
    .pQueuePriorities = &priority,
};

const char *devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

VkDeviceCreateInfo deviceInfo = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .pNext = NULL,
    .flags = 0,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &queueInfo,
    .enabledLayerCount = 0,
    .ppEnabledLayerNames = NULL,
    .enabledExtensionCount = 1,
    .ppEnabledExtensionNames = devExts,
    .pEnabledFeatures = NULL,
};

VkDevice device = VK_NULL_HANDLE;
VkResult res = vkCreateDevice(chosen, &deviceInfo, NULL, &device);
if (res != VK_SUCCESS) {
    fprintf(stderr, "vkCreateDevice failed: %d\n", res);
    return 1;
}

VkQueue graphicsQueue = VK_NULL_HANDLE;
vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
```

`vkGetDeviceQueue` takes the family index and the queue position within the family. Queues need no explicit destruction; `vkDestroyDevice` releases them. Resolve entry points with `vkGetInstanceProcAddr` or `vkGetDeviceProcAddr`, which return a generic `PFN_vkVoidFunction` you must cast to the declared function type before calling. Handles and other device-scoped pointers belong to the `VkDevice` that created them and must never be mixed across devices. Creation flows inward and destruction flows outward in reverse: call `vkDeviceWaitIdle` before teardown so in-flight work finishes, destroy the logical device, then destroy the instance.

If the GPU hangs, overheats, or is removed, work returns `VK_ERROR_DEVICE_LOST`. Loss strikes the logical device, not the physical device handle: the `VkPhysicalDevice` still describes hardware, but the lost `VkDevice` and every queue from it are unusable. There is no reset call; destroy the lost device and create a new logical device, then re-create everything derived from it, to recover.

## 6. Where this guide goes next

Keep the skeleton visible while later chapters add muscle. Chapter 3 turns the validation layer you enabled here into a debugging workflow with `VK_EXT_debug_utils`, message severity, and breakpoints on validation output. Chapter 4 takes the memory properties you queried here and turns them into allocation, heaps, host-visible versus device-local choices, and the create, query, allocate, bind rhythm every buffer and image follows. Chapter 5 takes up queues and command buffers, Chapter 6 moves data, Chapter 7 presents through the swapchain, Chapter 8 covers shaders, Chapter 10 builds graphics pipelines, and Chapter 15 synchronizes frames with fences versus semaphores; these group topics, not a strict build sequence.

---

### Recap

- The first program runs instance, layers/extensions, physical device, queue families, logical device, queues, then surface, swapchain, pools, resources, shaders, pipeline, and the acquire, record, submit, present loop.
- Query before you enable, handle `VK_INCOMPLETE`, and enable layers only on the instance with `VK_LAYER_KHRONOS_validation` for development.
- `VkInstance` joins the loader with an API version; `VkPhysicalDevice` is a query-only discovery handle; `VkDevice` plus `VkQueue` is the owned channel you submit to.
- Queues come from families you chose at device creation; `vkGetDeviceQueue` fetches them and `vkDestroyDevice` releases them.
- Device loss kills the logical device and its queues, never the physical device description; recovery means re-creating the device and everything derived from it.

### Exercises

1. **Dump and gate.** Extend `list_instance_layers` to also print global extensions, then gate `vkCreateInstance`: abort with a clear message if `VK_LAYER_KHRONOS_validation` or `VK_KHR_wayland_surface` is missing. Run with and without validation installed and compare.
2. **Choose deliberately.** Print every GPU name, type, and queue-family flags, then change selection to reject any GPU without a graphics family and without `VK_KHR_swapchain`. Which GPU wins on your machine, and which handle from this chapter would break first if you skipped its query?
