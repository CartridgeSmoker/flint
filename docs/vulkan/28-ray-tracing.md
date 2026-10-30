# Chapter 28: Ray Tracing Foundations

> **Objectives:** Explain BLAS and TLAS acceleration structures; describe ray pipeline stages (raygen, miss, hit, callable); create a ray tracing pipeline and shader binding tables; build BLAS and TLAS with correct buffers, addresses, and barriers. Chapter 29 builds ray-traced shadows on this pipeline, and Chapter 30 builds diffuse global illumination on it.
>
> **Flint note:** Flint does not ray trace its main viewport yet - the raster path remains primary for CAD interaction. Use this chapter as the foundation for inspection aids: ray-traced selection picking, clearance checks, and reference shadows that validate the raster result.

## 28.1 Ray tracing needs two new ideas

Rasterization asks, for each triangle, which pixels it covers. Ray tracing inverts the question: for each ray, which triangle does it hit. Testing every ray against every triangle is far too slow, so Vulkan ray tracing rests on two constructs with no raster equivalent.

The first is the acceleration structure: a bounding-volume hierarchy (BVH) so rays skip whole meshes at once. Parent nodes store only boxes; only leaves store geometry. The driver builds and traverses this tree, usually with hardware help on recent GPUs.

The second is the ray pipeline: shaders that invoke other shaders. Raygen fires rays with `traceRayEXT`; traversal dispatches to miss, hit, or callable shaders. A shader binding table (SBT) picks which shader runs.

Enabling this needs explicit extensions and features:

```c
const char *exts[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};
```

plus `VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress`, `VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure`, and `VkPhysicalDeviceRayTracingPipelineFeaturesKHR::rayTracingPipeline`. Query limits once at startup:

```c
VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR,
};
VkPhysicalDeviceProperties2 props2 = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
    .pNext = &rtProps,
};
vkGetPhysicalDeviceProperties2(physDev, &props2);
// rtProps.shaderGroupHandleSize, rtProps.shaderGroupBaseAlignment,
// rtProps.maxRayRecursionDepth, rtProps.maxRayDispatchInvocationCount
```

You will need `shaderGroupHandleSize` and `shaderGroupBaseAlignment` when sizing the SBT in Section 28.4.

## 28.2 BLAS holds geometry, TLAS holds placements

Vulkan splits the hierarchy in two. A bottom-level acceleration structure (BLAS) holds triangles: vertex and index buffers plus formats. A top-level acceleration structure (TLAS) holds instances: references to BLASes plus a transform, mask, and flags per instance. Model a wheel mesh once as a BLAS, then place it four times in the TLAS with four matrices.

Each mesh contributes one `VkAccelerationStructureGeometryKHR` entry of triangle type:

```c
VkAccelerationStructureGeometryKHR geo = {
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
    .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
    .flags = isOpaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0,
};
geo.geometry.triangles = (VkAccelerationStructureGeometryTrianglesDataKHR){
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
    .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
    .vertexData = { .deviceAddress = vertexAddr + vertexOffset },
    .vertexStride = 3 * sizeof(float),
    .maxVertex = vertexCount - 1, // spec convention: highest indexable vertex, not the count
    .indexType = mesh.uses16BitIndices ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
    .indexData = { .deviceAddress = indexAddr },
};
```

`vertexAddr` comes from `vkGetBufferDeviceAddress`, which needs `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` on the vertex buffer. Each geometry also needs a build-range entry:

```c
VkAccelerationStructureBuildRangeInfoKHR range = {
    .primitiveCount = indexCount / 3, // triangles, not vertices
    .primitiveOffset = 0,
    .firstVertex = 0,
    .transformOffset = 0,
};
```

Note the count is triangles: `indexCount / 3` for indexed meshes, `vertexCount / 3` when unindexed.

## 28.3 Build the BLAS, then the TLAS

Building is two phases: query sizes, then allocate storage plus scratch and record the build:

```c
VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
    .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
    .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
    .geometryCount = geoCount,
    .pGeometries = geos,
};

uint32_t *maxPrims = malloc(geoCount * sizeof(uint32_t));
for (uint32_t i = 0; i < geoCount; ++i)
    maxPrims[i] = ranges[i].primitiveCount;

VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
};
vkGetAccelerationStructureBuildSizesKHR(dev,
    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
    &buildInfo, maxPrims, &sizeInfo);
```

> **Build flags:** this guide uses `VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR` to favor trace performance for a static CAD scene. A reference source may instead pass `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR` to allow in-place updates and a post-build compaction pass that trades extra build work for smaller storage. Pick fast-trace for static geometry; pick allow-update/allow-compaction when meshes animate per frame or memory pressure dominates.

Allocate storage (`VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR`, `accelerationStructureSize`) and scratch (`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, `buildScratchSize`). Create the handle bound to storage, then record:

```c
VkAccelerationStructureCreateInfoKHR createInfo = {
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
    .buffer = blasStorage, .offset = 0,
    .size = sizeInfo.accelerationStructureSize,
    .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
};
VkAccelerationStructureKHR blas;
vkCreateAccelerationStructureKHR(dev, &createInfo, NULL, &blas);

buildInfo.dstAccelerationStructure = blas;
buildInfo.scratchData.deviceAddress = scratchAddr;

const VkAccelerationStructureBuildRangeInfoKHR *pRanges = ranges;
vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRanges);
```

Scratch may be reused after the build; storage must live as long as the BLAS is traced.

The TLAS is the same shape with one instance array. Fill one entry per placement:

```c
VkAccelerationStructureInstanceKHR inst = { 0 };
inst.transform.matrix[0][0] = 1.0f;
inst.transform.matrix[1][1] = 1.0f;
inst.transform.matrix[2][2] = 1.0f;
inst.mask = 0xFF;
inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
inst.accelerationStructureReference = blasDeviceAddress;
```

`blasDeviceAddress` comes from `vkGetAccelerationStructureDeviceAddressKHR` on the finished BLAS, not the buffer address. Upload instances to a device buffer with `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, point an instance-type geometry at it, and build with `.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR`.

Build the BLAS first and separate it from the TLAS build with a barrier or a separate submit, since the TLAS reads the finished BLAS:

```c
VkMemoryBarrier2KHR barrier = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR,
    .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
    .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
        | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
    .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
};
VkDependencyInfoKHR dep = {
    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
    .memoryBarrierCount = 1, .pMemoryBarriers = &barrier,
};
vkCmdPipelineBarrier2KHR(cmd, &dep);
```

> **Submit note:** a reference source wraps each build in a `submit_immediate` one-shot submit, which also separates BLAS from TLAS. The barrier above in the same command buffer is equally legal and usually cheaper: one submit, with the `ACCELERATION_STRUCTURE_BUILD_BIT_KHR` write made visible to the TLAS build and the later ray tracing shaders.

Independent BLAS builds (and, once their inputs are ready, multiple TLAS builds) have no data dependency on each other, so they can be recorded back-to-back in one command buffer or across threads and submitted in parallel; only the BLAS-then-TLAS edge needs the barrier or separate submit above.

> **Misconception: "The BLAS is the scene and the TLAS is optional."**
> It is the reverse. The TLAS is the scene; `traceRayEXT` always takes a TLAS. The BLAS is a reusable fragment with no world transform. To animate, rebuild or update only the TLAS and leave static BLASes alone.

## 28.4 Pipelines, groups, and the shader binding table

Ray shaders come in six stages: `raygen`, `miss`, `closest hit`, `any hit`, `intersection`, and `callable`. This guide needs three: raygen fires a ray per pixel, closest-hit returns red, miss returns the background.

```glsl
// raygen.rgen
#extension GL_EXT_ray_tracing : enable
layout(location = 0) rayPayloadEXT vec4 payload;
layout(binding = 1, set = 0) uniform accelerationStructureEXT tlas;
layout(binding = 2, set = 0, rgba8) uniform image2D outImage;

void main() {
    vec2 uv = (vec2(gl_LaunchIDEXT.xy) + 0.5) / vec2(gl_LaunchSizeEXT.xy);
    vec3 origin = cameraPos, dir = rayDirFromUV(uv, invViewProj, cameraPos);
    traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFF,
        0, 0, 0, origin, 0.0, dir, 1000.0, 0);
    imageStore(outImage, ivec2(gl_LaunchIDEXT.xy), payload);
}
```

```glsl
// chit.rchit
#extension GL_EXT_ray_tracing : enable
layout(location = 0) rayPayloadInEXT vec4 payload;
void main() { payload = vec4(1.0, 0.0, 0.0, 1.0); }
```

```glsl
// miss.rmiss
#extension GL_EXT_ray_tracing : enable
layout(location = 0) rayPayloadInEXT vec4 payload;
void main() { payload = vec4(0.05, 0.07, 0.10, 1.0); }
```

`rayDirFromUV` unprojects the pixel through the inverse view-projection matrix. The Y-flip converts the top-left launch origin to OpenGL-style NDC (Y up):

```glsl
vec3 rayDirFromUV(vec2 uv, mat4 invViewProj, vec3 camPos) {
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - (uv.y * 2.0 - 1.0)); // Y-flip: row 0 is NDC top
    vec4 world = invViewProj * vec4(ndc, 1.0, 1.0); world /= world.w;
    return normalize(world.xyz - camPos);
}
```

The `location = 0` payload must match between raygen and hit/miss. The `sbtRecordOffset`, `sbtRecordStride`, and `missIndex` arguments index the SBT; zeros are correct with one entry per table.

Pipeline creation groups stages into shader groups. General stages (raygen, miss, callable) each get a `GENERAL` group; triangle hit shaders share a `TRIANGLES_HIT_GROUP`. Declare the stages in raygen, closest-hit, miss order so the indices below read literally:

```c
VkPipelineShaderStageCreateInfo stages[3] = {
    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR, .module = rgenModule, .pName = "main" }, // stages[0]: raygen
    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, .module = chitModule, .pName = "main" }, // stages[1]: closest hit
    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_MISS_BIT_KHR, .module = missModule, .pName = "main" }, // stages[2]: miss
};

VkRayTracingShaderGroupCreateInfoKHR groups[3] = { 0 };
// group 0 -> stages[0] (raygen), group 1 -> stages[2] (miss), group 2 -> stages[1] (chit)
groups[0] = (VkRayTracingShaderGroupCreateInfoKHR){
    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
    .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
    .generalShader = 0,
    .closestHitShader = VK_SHADER_UNUSED_KHR,
    .anyHitShader = VK_SHADER_UNUSED_KHR,
    .intersectionShader = VK_SHADER_UNUSED_KHR,
};
groups[1] = (VkRayTracingShaderGroupCreateInfoKHR){
    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
    .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
    .generalShader = 2,
    .closestHitShader = VK_SHADER_UNUSED_KHR,
    .anyHitShader = VK_SHADER_UNUSED_KHR,
    .intersectionShader = VK_SHADER_UNUSED_KHR,
};
groups[2] = (VkRayTracingShaderGroupCreateInfoKHR){
    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
    .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
    .generalShader = VK_SHADER_UNUSED_KHR,
    .closestHitShader = 1,
    .anyHitShader = VK_SHADER_UNUSED_KHR,
    .intersectionShader = VK_SHADER_UNUSED_KHR,
};

VkRayTracingPipelineCreateInfoKHR pipeInfo = {
    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
    .stageCount = 3, .pStages = stages,
    .groupCount = 3, .pGroups = groups,
    .maxPipelineRayRecursionDepth = 1,
    .layout = pipeLayout,
};
vkCreateRayTracingPipelinesKHR(dev, VK_NULL_HANDLE,
    VK_NULL_HANDLE, 1, &pipeInfo, NULL, &rtPipe);
```

`maxPipelineRayRecursionDepth = 1` forbids child rays; raise it only when hit shaders call `traceRayEXT` themselves. Clamp it to `rtProps.maxRayRecursionDepth`, and clamp the `vkCmdTraceRaysKHR` width x height x depth product to `rtProps.maxRayDispatchInvocationCount`.

The SBT is the lookup table the hardware consults on every hit or miss. Fetch opaque group handles, then copy each into its own device buffer with `VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`. Each entry is padded to `shaderGroupBaseAlignment`:

```c
uint32_t handleSize = rtProps.shaderGroupHandleSize;
uint32_t baseAlign = rtProps.shaderGroupBaseAlignment;
uint32_t stride = (handleSize + baseAlign - 1) & ~(baseAlign - 1);

uint32_t groupCount = 3;
size_t handlesSize = (size_t)groupCount * handleSize;
uint8_t *handles = malloc(handlesSize);
vkGetRayTracingShaderGroupHandlesKHR(dev, rtPipe, 0, groupCount,
    handlesSize, handles);

// One device-local buffer per table (raygen, miss, hit = groups 0, 1, 2).
// Each buffer holds a single stride-sized entry: handle bytes + zero padding.
VkBuffer sbtBuf[3];
VkDeviceAddress sbtAddr[3];
for (uint32_t i = 0; i < groupCount; ++i) {
    VkBufferCreateInfo sbtInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = stride,
        .usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR
               | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    vkCreateBuffer(dev, &sbtInfo, NULL, &sbtBuf[i]);
    // allocate + bind device memory here (omitted), then upload:
    uint8_t *mapped = NULL;
    vkMapMemory(dev, sbtMem[i], 0, stride, 0, (void **)&mapped);
    memset(mapped, 0, stride);
    memcpy(mapped, handles + (size_t)i * handleSize, handleSize);
    vkUnmapMemory(dev, sbtMem[i]);
    VkBufferDeviceAddressInfo addrInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = sbtBuf[i],
    };
    sbtAddr[i] = vkGetBufferDeviceAddress(dev, &addrInfo);
}
free(handles);
```

Dispatch binds the pipeline and points four address regions at those buffers
(`sbtAddr[0]` is raygen/group 0, `sbtAddr[1]` is miss/group 1, `sbtAddr[2]` is hit/group 2):

```c
VkStridedDeviceAddressRegionKHR raygenRegion = {
    .deviceAddress = sbtAddr[0], .stride = stride, .size = stride,
};
VkStridedDeviceAddressRegionKHR missRegion = {
    .deviceAddress = sbtAddr[1], .stride = stride, .size = stride,
};
VkStridedDeviceAddressRegionKHR hitRegion = {
    .deviceAddress = sbtAddr[2], .stride = stride, .size = stride,
};
VkStridedDeviceAddressRegionKHR callableRegion = { 0 };

vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rtPipe);
vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
    pipeLayout, 0, 1, &descSet, 0, NULL);
vkCmdTraceRaysKHR(cmd, &raygenRegion, &missRegion,
    &hitRegion, &callableRegion, width, height, 1);
```

Width and height are the launch size (`gl_LaunchSizeEXT`); depth stays 1. The descriptor set binds the TLAS (`VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` with `VkWriteDescriptorSetAccelerationStructureKHR` in `pNext`) and the output storage image. An empty callable region is valid when no callables are used.

## Recap

- Ray tracing adds traversal plus dispatch: structures skip empty space, the SBT picks shaders per hit or miss.
- BLAS stores triangles; TLAS stores instances. `traceRayEXT` always traverses a TLAS.
- Build order: geometry plus ranges, size query, storage plus scratch, create, build, barrier before TLAS and tracing.
- Raygen fires rays, closest-hit shades them, miss colors the background; keep payload locations matched.
- Groups map stages to dispatch slots; the SBT holds aligned handles, one table per category, dispatched with `vkCmdTraceRaysKHR`.

## Exercises

1. **SBT stride audit.** Query `shaderGroupHandleSize` and `shaderGroupBaseAlignment`, compute the padded stride, and dump the three SBT buffers to confirm padding is zero. Explain what breaks if raw handle size is used as stride.
2. **Instance mask filter.** Add a second instance with `mask = 0x02`, dispatch with `cullMask = 0xFF` then `0x01`, and record which pixels change. State the rule relating ray mask and instance mask.
