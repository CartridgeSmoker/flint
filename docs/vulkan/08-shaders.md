# Chapter 8: Shaders and Compute

> **Objectives:**
> - Compile GLSL offline to SPIR-V and read its header, magic, and entry points.
> - Load SPIR-V files into `VkShaderModule` objects with a hardened loader.
> - Tune one shader into many pipelines with specialization constants.
> - Create compute pipelines, bind them, and dispatch workgroups.
> - Create, save, and reload a `VkPipelineCache` with a vendor and UUID header check.

Previous chapters gave you memory, images, and a window. This chapter adds the workers: small programs that run on the device, and pipelines that say which program runs and with which built-in constants. The story is deliberately narrow. You write text, ship a binary, wrap it in a module, freeze it into a pipeline, and launch it. Resources such as descriptor sets get only a passing mention here; they take center stage in Chapter 9, and full graphics pipelines follow in Chapter 10.

## 1. GLSL in, SPIR-V out

Vulkan never sees your shader source. It reads only SPIR-V, a binary stream of 32-bit words in single-static-assignment form where every value is written once. You rarely read it by hand, but its shape explains several API rules.

Write shaders in GLSL under the Vulkan profile. That means `#version 450` and explicit decorations such as `layout(local_size_x = 64) in;` for compute shaders. Then compile offline with the reference compiler:

```sh
glslangValidator -V simple.comp -o simple.spv
```

Here `-V` requests Vulkan SPIR-V output and `.comp` marks a compute shader. Validate with `spirv-val`, and inspect with `spirv-dis` when curious. A minimal compute shader disassembles to `OpCapability Shader`, `OpMemoryModel Logical GLSL450`, `OpEntryPoint GLCompute %4 "main"`, and an `OpExecutionMode` line recording the local workgroup size.

Every `.spv` file starts with a fixed header. The first word is the magic number `0x07230203`, which also reveals byte order. The words that follow carry version, generator id, and bound. The blob is verbose, even a do-nothing shader lands near 200 bytes, but it compresses well and parses deterministically. Treat `.spv` files as build artifacts: compile at build time and never link a GLSL compiler into your runtime.

> **Flint note:** Flint compiles all GLSL to `.spv` at build time with `glslangValidator` and loads the binaries at startup. The runtime has no shader compiler dependency, which keeps startup predictable.

## 2. Shader modules wrap SPIR-V for the driver

A `VkShaderModule` wraps one SPIR-V blob. The blob may hold several entry points, and code generation waits until pipeline creation, when the entry point and pipeline layout are known. `codeSize` is measured in bytes and must be a multiple of 4; `pCode` must point to aligned 32-bit words.

File loading looks trivial, which is why it deserves care. A hardened loader checks the open, the size, the alignment, the magic, the read, and the Vulkan call:

```c
static VkShaderModule
loadShaderModule(VkDevice device, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return VK_NULL_HANDLE;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return VK_NULL_HANDLE;
    }
    long size = ftell(fp);
    rewind(fp);
    if (size <= 0 || (size % 4) != 0) {
        fclose(fp);
        return VK_NULL_HANDLE;
    }

    uint32_t *code = malloc((size_t)size);
    if (code == NULL) {
        fclose(fp);
        return VK_NULL_HANDLE;
    }
    size_t got = fread(code, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        free(code);
        return VK_NULL_HANDLE;
    }
    if (code[0] != 0x07230203u) {
        free(code);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo moduleInfo = { 0 };
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = (size_t)size;
    moduleInfo.pCode = code;

    VkShaderModule module = VK_NULL_HANDLE;
    VkResult res = vkCreateShaderModule(device, &moduleInfo, NULL, &module);
    free(code);
    if (res != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}
```

Each module joins pipeline creation through a `VkPipelineShaderStageCreateInfo` that names one entry point, almost always `"main"`:

```c
VkPipelineShaderStageCreateInfo stage = { 0 };
stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
stage.module = mod;
stage.pName = "main";
```

Pipelines keep what they need from the module, so a module may be destroyed after pipeline creation. Do not destroy it while another thread is still building a pipeline from it. Destroy finished modules with `vkDestroyShaderModule`.

> **Misconception:**
>
> "Vulkan compiles my GLSL at runtime, so I can pass source text to the driver." No. The driver reads only SPIR-V words, and compilation of GLSL happens on your machine before the program ships. What the driver does at pipeline creation is a second step: translating that SPIR-V plus entry point plus constants into device code. Keep the two steps separate and both become easier to debug.

## 3. One shader, many pipelines with specialization constants

Specialization constants let one SPIR-V binary produce many pipelines. Values are fixed at pipeline creation, so the driver can fold arithmetic, prune branches, and unroll loops around them. They run faster than an equivalent uniform and avoid one shader file per tuning.

Declare each tunable with an id and a default in GLSL:

```glsl
layout(constant_id = 0) const int TILE_W = 16;
layout(constant_id = 1) const bool USE_SDF = false;
```

In SPIR-V these become `OpSpecConstant` entries with `SpecId` decorations. Override the defaults on the host with `VkSpecializationInfo`, which maps constant ids to byte ranges in a data blob (`offsetof` needs `#include <stddef.h>`):

```c
#include <stddef.h>
struct SpecData { int tileW; VkBool32 useSdf; } specData = { 32, VK_TRUE };
VkSpecializationMapEntry entries[2] = {
    { 0, offsetof(struct SpecData, tileW), sizeof(specData.tileW) },
    { 1, offsetof(struct SpecData, useSdf), sizeof(specData.useSdf) },
};
VkSpecializationInfo spec = { 0 };
spec.mapEntryCount = 2;
spec.pMapEntries = entries;
spec.dataSize = sizeof(specData);
spec.pData = &specData;
stage.pSpecializationInfo = &spec;
```

Ids left out of the map keep their GLSL defaults. A good rule of thumb: specialize code shape such as tile widths, loop counts, and feature toggles, and leave per-dispatch values such as cursor position to push constants or uniform data later.

## 4. Compute pipelines and dispatch

A compute pipeline freezes three things together: one shader stage, one pipeline layout, and the specialization values above. The pipeline layout itself, grouping descriptor set layouts and push-constant ranges, is covered in Chapter 9; here it arrives as a ready handle:

```c
VkComputePipelineCreateInfo pipe = { 0 };
pipe.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
pipe.stage = stage;
pipe.layout = pipelineLayout;

VkPipeline pipeline = VK_NULL_HANDLE;
vkCreateComputePipelines(device, pipelineCache, 1, &pipe, NULL, &pipeline);
```

Creation is often the most expensive startup call because device code generation happens here. That cost is exactly what the cache in the next section amortizes.

Launching work takes three commands. Bind the pipeline, bind the descriptor sets that feed it, and dispatch a grid of workgroups:

```c
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
    pipelineLayout, 0, 2, setsToBind, 0, NULL);
vkCmdDispatch(cmd, groupX, groupY, groupZ);
```

`vkCmdDispatch` launches `groupX x groupY x groupZ` workgroups, each of the shader's `local_size`. Stay within `maxComputeWorkGroupCount`, `maxComputeWorkGroupSize`, and `maxComputeWorkGroupInvocations`; exceeding them is undefined behavior that validation layers catch. For GPU-driven counts, fill a `VkDispatchIndirectCommand` buffer and call `vkCmdDispatchIndirect(cmd, buffer, offset)`, with `offset` a multiple of 4 and the buffer created with `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`. Writes to that buffer before the dispatch need a memory barrier. A pipeline must outlive every command buffer that records it, so destroy it only after in-flight work finishes.

## 5. The pipeline cache makes startup pay once

A `VkPipelineCache` captures compilation results across pipelines and across runs. Create one at startup, pass it to every pipeline creation call, and save its contents to disk on exit:

```c
VkPipelineCacheCreateInfo cacheInfo = { 0 };
cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
cacheInfo.initialDataSize = blobSize; /* 0 on first run */
cacheInfo.pInitialData = blob;        /* NULL on first run */

VkPipelineCache cache = VK_NULL_HANDLE;
vkCreatePipelineCache(device, &cacheInfo, NULL, &cache);
```

Saving uses the two-step query: ask for the size, allocate, then fetch:

```c
size_t size = 0;
VkResult res = vkGetPipelineCacheData(device, cache, &size, NULL);
if (res != VK_SUCCESS || size == 0) { /* no data to save */ }
else {
    void *blob = malloc(size);
    if (blob) {
        res = vkGetPipelineCacheData(device, cache, &size, blob);
        if (res == VK_SUCCESS) { /* write blob (size bytes) to disk */ }
        free(blob);
    }
}
```

The blob is driver-private except for its 32-byte header: length, version (currently 1), `vendorID`, `deviceID`, and the 16-byte `pipelineCacheUUID`. Before seeding a cache from disk, compare all three values against `VkPhysicalDeviceProperties` and discard the file on any mismatch. When several threads build pipelines, give each thread its own cache and merge them with `vkMergePipelineCaches` before the single save; this avoids lock contention. Pipelines outlive the cache.

That completes the compute story: text compiled offline, binary checked and wrapped, constants baked in, pipeline frozen, grid launched, compilation effort saved for next run. Chapter 9 wires resources into this picture with descriptor sets, layouts, pools, and push constants, and Chapter 10 extends pipelines from compute to graphics with vertex input and fixed-function state; Chapter 11 builds the render pass and framebuffers.

---

### Recap

- SPIR-V is the only language the driver reads; compile GLSL offline with `glslangValidator -V`, check with `spirv-val` or `spirv-dis`, and look for the `0x07230203` magic.
- `VkShaderModule` wraps one SPIR-V blob; validate size, alignment, magic, and every call, and name one entry point per stage at pipeline creation.
- Specialization constants (`constant_id`, `VkSpecializationInfo`) bake tuning values into compilation for folding and pruning from a single binary.
- Compute pipelines freeze module plus layout; `vkCmdDispatch` launches a grid of `local_size` workgroups within the device workgroup limits.
- `VkPipelineCache` persists compilation across runs; verify `vendorID`, `deviceID`, and `pipelineCacheUUID` before reloading, and merge per-thread caches before saving.

### Exercises

1. **Specialize one shader twice.** Write a compute shader with `layout(constant_id = 0) const int TILE_W = 16;`, compile it once with `glslangValidator -V`, then create two compute pipelines with `TILE_W` of 16 and 32. Dispatch both over the same buffer and use `spirv-dis` plus your timings to explain which index math or loop the larger tile lets the compiler fold.
2. **Cache round-trip.** Create a `VkPipelineCache`, build your compute pipeline against it, and save the blob with the two-call `vkGetPipelineCacheData` pattern. Restart the process, reload the file after checking the header UUID, and time cold versus warm creation. Then corrupt one UUID byte and confirm your loader discards the cache.
