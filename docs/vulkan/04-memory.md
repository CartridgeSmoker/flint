# Chapter 4: Memory and Resources

> **Objectives:**
> - Distinguish host memory from device memory, and heaps from memory types.
> - Select a memory type from `memoryTypeBits` plus property flags.
> - Allocate with `vkAllocateMemory`, respect alignment, and bind before use.
> - Map safely; flush and invalidate non-coherent mappings.
> - Manage lifetimes: destroy views, resources, then memory after idle.

Creating an object is not giving it storage. A `VkBuffer` or `VkImage` is hollow until device memory backs it. The driver reports what it has, each resource reports what it needs, and you choose. Commands, covered in Chapter 5, only become meaningful once they reference bound memory; presentation, covered in Chapter 7, is the one exception where images arrive already bound.

## 1. Host memory versus device memory

**Host memory** is ordinary CPU memory for driver bookkeeping: object tables, layer state, command recording scratch. It comes from the default allocator unless you pass custom `VkAllocationCallbacks` through `pAllocator`; passing `NULL` is correct for now. A custom allocator receives your `pUserData` state pointer on every callback, must honor the requested alignment, and sees each allocation tagged with a `VkSystemAllocationScope` ordered from innermost `COMMAND` through `OBJECT` and `CACHE` out to `DEVICE` and `INSTANCE`. The paired internal-allocation notification callbacks are accounting-only and must either both be `NULL` or both be set.

**Device memory** is any memory the device can touch: dedicated VRAM, a host-visible window into VRAM, host RAM reached over PCIe, or the shared pool on unified-memory laptops and phones. Never equate it with VRAM. Ask the device with `vkGetPhysicalDeviceMemoryProperties`:

```c
VkPhysicalDeviceMemoryProperties memProps = { 0 };
vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
// memProps.memoryHeaps[] gives sizes; memProps.memoryTypes[] gives flags plus heapIndex.
```

Physical pools are **heaps** (`VkMemoryHeap`): a size plus flags such as `VK_MEMORY_HEAP_DEVICE_LOCAL_BIT`. Each heap offers one or more **memory types** (`VkMemoryType`), each with property flags describing how that view onto the heap behaves:

- `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`: fast for the device, often unmappable on discrete GPUs. Unified-memory parts may expose DEVICE_LOCAL|HOST_VISIBLE together — always test propertyFlags.
- `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`: mappable with `vkMapMemory`.
- `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`: the driver handles cache consistency.
- `VK_MEMORY_PROPERTY_HOST_CACHED_BIT`: host reads are cached; faster CPU reads.
- `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT`: commitment deferred until first use; for transient attachments only, never combined with host visibility.

Two types may share one heap with different flags. Read both the heap index and the flags before deciding. Staging buffers want host-visible and preferably coherent; GPU-only vertex buffers and textures want device-local; transient depth attachments may want lazily allocated plus device-local.

## 2. Choosing a memory type

A resource never names a heap. `vkGetBufferMemoryRequirements` or `vkGetImageMemoryRequirements` returns a `VkMemoryRequirements` with `size`, `alignment`, and `memoryTypeBits`: one bit per acceptable type index. Intersect those bits with the properties you need:

```c
uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                        uint32_t typeBits,
                        VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties props = { 0 };
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);

    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) == 0)
            continue;
        if ((props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return UINT32_MAX;
}
```

Use device-local for GPU-only textures and vertex buffers, host-visible plus coherent when available for staging and readback. If the ideal combination is missing, relax preferences before requirements.

> **Misconception:** "Memory types are ranked, so pick index zero."
>
> Index order means nothing. Bit zero is just the first type the driver listed. Every resource carries its own `memoryTypeBits` mask, and only the intersection of that mask with the flags you need is valid. Match them every time, and treat `UINT32_MAX` from `findMemoryType` as no suitable type, not as a fallback index.

## 3. Allocate, then bind before use

Creation describes; binding backs. The `usage` field declares intent up front and cannot be widened later, so choose it carefully. Buffers take bits such as `VK_BUFFER_USAGE_VERTEX_BUFFER_BIT`, `VK_BUFFER_USAGE_INDEX_BUFFER_BIT`, `VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT`, `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`, `VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT`, `VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT`, and `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`, plus `VK_BUFFER_USAGE_TRANSFER_SRC_BIT` and `VK_BUFFER_USAGE_TRANSFER_DST_BIT` for copies. Images take bits such as `VK_IMAGE_USAGE_SAMPLED_BIT`, `VK_IMAGE_USAGE_STORAGE_BIT`, `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`, `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT`, and `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT`, plus `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` and `VK_IMAGE_USAGE_TRANSFER_DST_BIT`.

The rhythm is fixed: create the handle, query requirements, find a type, allocate, bind at an aligned offset.

```c
VkBufferCreateInfo bufferInfo = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .pNext = NULL,
    .size = 1024 * 1024,
    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
};

VkBuffer buffer = VK_NULL_HANDLE;
VkResult res = vkCreateBuffer(device, &bufferInfo, NULL, &buffer);
if (res != VK_SUCCESS) {
    fprintf(stderr, "vkCreateBuffer failed: %d\n", res);
    return 1;
}

VkMemoryRequirements req = { 0 };
vkGetBufferMemoryRequirements(device, buffer, &req);
uint32_t typeIndex = findMemoryType(physicalDevice, req.memoryTypeBits,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

VkMemoryAllocateInfo allocInfo = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .pNext = NULL,
    .allocationSize = req.size,
    .memoryTypeIndex = typeIndex,
};

VkDeviceMemory memory = VK_NULL_HANDLE;
res = vkAllocateMemory(device, &allocInfo, NULL, &memory);
if (res != VK_SUCCESS) {
    // VK_ERROR_OUT_OF_DEVICE_MEMORY or VK_ERROR_TOO_MANY_OBJECTS.
    return 1;
}

res = vkBindBufferMemory(device, buffer, memory, 0);
if (res != VK_SUCCESS) {
    // Offset broke alignment, or the allocation is too small.
    return 1;
}
```

Images follow the same rhythm with a richer descriptor. Prefer `VK_IMAGE_TILING_OPTIMAL` unless the host must write texels directly; linear tiling is readable but slower and not always supported. Create in `VK_IMAGE_LAYOUT_UNDEFINED` when contents may be discarded, or `VK_IMAGE_LAYOUT_PREINITIALIZED` when host-written data must survive, then transition with a pipeline barrier before use. Other layouts you will meet soon include `VK_IMAGE_LAYOUT_GENERAL`, `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`, `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`, `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL`, `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`, `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`, `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL`, and `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`.

```c
VkImageCreateInfo imageInfo = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .pNext = NULL,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = VK_FORMAT_R8G8B8A8_UNORM,
    .extent = { 1024, 1024, 1 },
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
};

VkImage image = VK_NULL_HANDLE;
res = vkCreateImage(device, &imageInfo, NULL, &image);
// Query with vkGetImageMemoryRequirements, allocate, then vkBindImageMemory.
```

Here `mipLevels` of 1 and `arrayLayers` of 1 mean one full-resolution 2D image: the right starting point. Raise them only when you need mip chains, arrays, or cubemaps, staying within `maxImageDimension2D` and `maxImageArrayLayers`. Check format support with `vkGetPhysicalDeviceFormatProperties` before committing.

Two limits shape allocation strategy. **Alignment:** each resource inside an allocation must sit at a multiple of its `requirements.alignment`. **Count:** `VkPhysicalDeviceLimits::maxMemoryAllocationCount`, at least 4096, bounds live `VkDeviceMemory` objects, not resources. Exceeding it fails with `VK_ERROR_TOO_MANY_OBJECTS`, so allocate a few large blocks and suballocate inside them rather than one allocation per buffer. Each resource binds exactly once at a correct offset covering `offset + size`; sparse binding is the paged-memory exception and is out of scope for this guide.

For large, long-lived images, prefer a **dedicated allocation** through a `pNext` chain, which tells the driver the block backs exactly one resource and can unlock tiling paths:

```c
VkMemoryRequirements imgReq = { 0 };
vkGetImageMemoryRequirements(device, image, &imgReq);
uint32_t imgType = findMemoryType(physicalDevice, imgReq.memoryTypeBits,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

VkMemoryDedicatedAllocateInfo dedicated = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    .pNext = NULL,
    .image = image,
    .buffer = VK_NULL_HANDLE,
};

VkMemoryAllocateInfo dedicatedAlloc = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .pNext = &dedicated,
    .allocationSize = imgReq.size,
    .memoryTypeIndex = imgType,
};
```

`pNext` is Vulkan's extension mechanism: each chained structure carries its own `sType`, terminated by `NULL`. Lazily allocated memory pairs with `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT`; observe its real backing with `vkGetDeviceMemoryCommitment`.

Swapchain images break the rhythm on purpose. They arrive pre-bound through `vkGetSwapchainImagesKHR`: the presentation engine owns their memory, so your program must never allocate, bind, or free it. Create views over them and present them, as Chapter 7 shows.

## 4. Views describe use, not storage

Raw buffers and images are not directly usable by shaders or attachments. A `VkBufferView` created with `VkBufferViewCreateInfo` describes a texel-buffer view; a `VkImageView` created with `VkImageViewCreateInfo` describes how an image is interpreted for sampling or as a framebuffer attachment. Image views are mandatory for sampling and for attachments. This guide treats views once, here: they hold no storage, they die before their parent, and everything about descriptors, sampling, and framebuffers in later chapters builds on them.

## 5. Mapped memory and coherency

Only `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT` types map. One live mapping per allocation: unmap before remapping, even for disjoint ranges, and never assume the pointer survives across maps. The function is `vkUnmapMemory` with a lowercase m.

```c
void *mapped = NULL;
res = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
if (res != VK_SUCCESS) {
    // Not host-visible, or the range is out of bounds.
    return 1;
}
memcpy(mapped, source, (size_t) req.size);

VkMappedMemoryRange range = {
    .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
    .pNext = NULL,
    .memory = memory,
    .offset = 0,
    .size = VK_WHOLE_SIZE,
};
res = vkFlushMappedMemoryRanges(device, 1, &range);
vkUnmapMemory(device, memory);
```

If the type lacks `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`, flush with `vkFlushMappedMemoryRanges` after host writes and invalidate with `vkInvalidateMappedMemoryRanges`, using the same `VkMappedMemoryRange` structure, after device writes. Coherent types let the driver do this for you. Either way, device-side visibility still needs barriers; flush and invalidate only reconcile host caches. Keep map offsets and flush and invalidate ranges aligned to `minMemoryMapAlignment`, at least 64 bytes. For linear-tiled images, query `vkGetImageSubresourceLayout` and stride rows by `rowPitch`, not by width times texel size.

## 6. Lifetimes: destroy in reverse, synchronize teardown

There is no unbind call; destroy implies it. Views die before parents: `vkDestroyBufferView` before `vkDestroyBuffer`, `vkDestroyImageView` before `vkDestroyImage`. Free the `VkDeviceMemory` with `vkFreeMemory` only after every resource bound to it is destroyed and no queued work references it, passing the same allocator to destroy that you passed to create.

All teardown needs external synchronization: stop submitting, wait with `vkDeviceWaitIdle` or fences, then destroy. The driver does not reference-count. Aliasing two resources over one range is legal only when their live ranges never overlap. Chapter 5 shows how recorded commands hold those references across submissions, which is why teardown always waits first.

> **Flint note:** Flint keeps one backend on one logical device: one device-local pool for GPU-only resources, one host-visible coherent pool for staging, and dedicated allocations only for large render targets. Swapchain images arrive pre-bound and are never allocated or freed by the app. A multi-device engine would shard pools per heap family; Flint trades that generality for fewer fragmentation paths.

## Recap

- Host memory feeds the driver; device memory feeds the device. Heaps are pools, types are behaviors on pools.
- Each resource reports `size`, `alignment`, and `memoryTypeBits`. Intersect with required flags in `findMemoryType`.
- Buffers and images are hollow until bound. Create, query, allocate, bind once at an aligned offset.
- Allocate a few large blocks and suballocate; `maxMemoryAllocationCount` bounds allocations, not resources. Dedicate large images via `pNext`.
- Swapchain images arrive pre-bound; never allocate, bind, or free their memory.
- Views describe use, not storage. Map only host-visible memory, one mapping at a time; flush writes and invalidate reads unless coherent.
- Destroy views, then resources, then memory, after idle.

## Exercises

1. **Bind a staging buffer end to end.** Create a 1 MiB buffer with `VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT`, pick a host-visible coherent type with `findMemoryType`, allocate, bind at zero with `VkResult` checks, map, write a pattern, flush, unmap, then destroy in reverse after `vkDeviceWaitIdle`. Print `size` and `alignment`: why does alignment constrain suballocation offsets rather than inflating a single allocation? Then place two such buffers in one allocation: which offsets satisfy both alignments, and how much memory is saved versus two allocations?
2. **Prove a wrong type fails usefully.** With validation on, bind a buffer to a device-local-only type and try to map it, then check whether any host-visible bit is set for an optimally tiled sampled image. Which failure does the layer catch, and which does `findMemoryType` returning `UINT32_MAX` catch first?
