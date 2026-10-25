# Chapter 9: Descriptors, Push Constants, and Bindless

> **Objectives:** Describe a shader interface with set layouts; expose sets and push ranges through a pipeline layout; size pools and allocate sets; fill sets with writes, copies, and `VK_WHOLE_SIZE`; use dynamic offsets; refresh a uniform buffer each frame; push per-draw data with a padded `std430` struct; build a bindless array with `partiallyBound` plus a dummy fallback; generate layouts by SPIR-V reflection.

Shaders declare numbered slots - set plus binding - and the application supplies the buffers, images, and samplers behind them. Push constants cover the opposite end: a few hot bytes with no descriptor. Bindless scales up: one large array bound once, indexed per draw. Pipelines wait for Chapter 10; sampled images get detail in Chapter 14.

## Set layouts describe the interface

A `VkDescriptorSetLayout` describes one set: binding numbers, types, counts, and stages. It must mirror `layout(set = ..., binding = ...)` in GLSL. Start with the classic pair: binding 0 as a vertex-stage uniform buffer for the MVP matrix, binding 1 as a fragment-stage sampler that Chapter 14 will fill:

```c
VkDescriptorSetLayoutBinding bindings[2] = { 0 };
bindings[0].binding = 0;
bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
bindings[0].descriptorCount = 1;
bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
bindings[1].binding = 1;
bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
bindings[1].descriptorCount = 1;
bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

VkDescriptorSetLayoutCreateInfo layoutInfo = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = 2, .pBindings = bindings,
};
vkCreateDescriptorSetLayout(device, &layoutInfo, NULL, &setLayout);
```

`descriptorCount` is the GLSL array length. Uniform blocks are small and fast; storage blocks are large, writable, and atomic. Respect `maxUniformBufferRange` and `minUniformBufferOffsetAlignment`.

Each binding also carries `pImmutableSamplers`, `NULL` here, so samplers arrive through writes. Point it at `VkSampler` handles when the sampler is fixed for the layout lifetime, and writes then supply only the view - simpler writes and earlier folding, at the cost of one layout per sampler combination. Destroy the layout after the pipeline layouts using it are gone.

## Pipeline layouts expose sets and push ranges

`VkPipelineLayout` lists the set layouts a pipeline may see, plus push-constant ranges, and is baked into the pipeline at creation:

```c
VkPipelineLayoutCreateInfo pipeInfo = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1, .pSetLayouts = &setLayout,
    .pushConstantRangeCount = 0, .pPushConstantRanges = NULL,
};
vkCreatePipelineLayout(device, &pipeInfo, NULL, &pipelineLayout);
```

Compatibility is prefix-based, and push ranges participate: layouts sharing a set prefix are compatible only when those sets are identical *and* push ranges match in count and content. Keep globals in set 0 so pipelines sharing it swap without rebinding, and keep push ranges stable across pipelines you mix.

> **Flint note:** Flint's UI materials follow this split. Glyph atlas and frame block live in low-numbered sets shared across widgets, per-widget tint travels in push constants, and one pipeline per text mode shares a layout prefix so material switches rebind only the tail set. The atlas itself lives in the bindless array below.

## Pools allocate, sets instantiate

Sets are never created directly; they come from a `VkDescriptorPool` sized up front by type and count:

```c
VkDescriptorPoolSize sizes[2] = {
    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
};
VkDescriptorPoolCreateInfo poolInfo = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes,
};
vkCreateDescriptorPool(device, &poolInfo, NULL, &pool);

VkDescriptorSetAllocateInfo allocInfo = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = pool, .descriptorSetCount = 1,
    .pSetLayouts = &setLayout,
};
vkAllocateDescriptorSets(device, &allocInfo, &set);
```

`maxSets` caps sets; `pPoolSizes` caps descriptors per type. Undersize either and allocation fails. Pass an array of layouts to allocate several sets at once. Free with `vkFreeDescriptorSets`; destroy the pool once nothing from it is still in flight.

## Writes, copies, whole size, dynamic offsets

Filling a set wires references, not bytes. Call `vkUpdateDescriptorSets` after allocation and before first use:

```c
VkDescriptorBufferInfo bufRef = { uniformBuf, 0, sizeof(glm::mat4) };
VkWriteDescriptorSet write = {
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
    .dstSet = set, .dstBinding = 0, .dstArrayElement = 0,
    .descriptorCount = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    .pBufferInfo = &bufRef,
};
vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
```

Writes carry `pBufferInfo`, `pImageInfo`, or `pTexelBufferView` by type. Copies ride in the same call via `VkCopyDescriptorSet` entries duplicating one slot into another, with writes applied before copies. Use `VK_WHOLE_SIZE` as a buffer range to bind from `offset` to the end of the buffer without computing the remainder.

For `_DYNAMIC` types, the write fixes the buffer while each bind supplies the offset via `pDynamicOffsets`. One set then serves many objects packed into a large buffer. Updates apply to subsequently recorded commands, never retroactively. Never update, free, or reset a set while in-flight work may read it; wait or use a second set.

> **Misconception:** "The descriptor copies the uniform data."
> No. It stores a reference - buffer plus offset plus range, or view plus sampler. Rewriting buffer bytes each frame needs no set update; only pointing elsewhere does. Rewiring a set the GPU still reads is a race, so wait or double-buffer first.

## Per-frame uniforms rewrite the buffer

The vertex shader reads the transform at binding 0:

```glsl
#version 450
layout (std140, binding = 0) uniform bufferVals { mat4 mvp; } myBufferVals;
layout (location = 0) in vec4 pos;
layout (location = 1) in vec4 inColor;
layout (location = 0) out vec4 outColor;
void main() {
    outColor = inColor;
    gl_Position = myBufferVals.mvp * pos;
    gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0;
}
```

The z remap keeps depth in `[0, w]`, which Chapter 10's `LESS_OR_EQUAL` assumes. Create the buffer with `VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT`, allocate coherent host-visible memory, point the set at it once, then animate by rewriting the mapped bytes - no set update:

```c
angle += 0.003f;
glm::mat4 mvp = proj * view *
    glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0, 1, 0));
memcpy(mapped, &mvp, sizeof(mvp));
```

Unmap only at teardown. Group by update rate - scene, model, per-draw - so hot data does not drag cool data. Memory correction: some sources request only `HOST_VISIBLE` and pay an invalidate/flush pair per update. The coherent allocation above may skip the flush, whereas non-coherent memory requires that pair. Bind after the pipeline:

```c
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
    pipelineLayout, 0, 1, &set, 0, NULL);
```

## Push constants carry a few hot bytes

Push constants skip descriptors: bytes in the layout, written from the command buffer with no barrier. At least 128 bytes are guaranteed - check `maxPushConstantsSize` - so they suit flags and tints, not matrices. Offsets and sizes are multiples of 4; name only reading stages in `stageFlags`. Because `std430` aligns a `vec4` to 16 bytes, pad the host struct to match:

```glsl
layout(push_constant) uniform Pushes { int mode; vec4 tint; } push;
```

```c
/* mode at 0, 12 bytes padding, tint at 16, 32 bytes total. */
struct { int mode; float _pad[3]; float tint[4]; } vals = {
    1, { 0, 0, 0 }, { 1, 1, 1, 1 }
};
VkPushConstantRange push = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(vals) };
VkPipelineLayoutCreateInfo pushPipeInfo = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1, .pSetLayouts = &setLayout,
    .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
};
vkCreatePipelineLayout(device, &pushPipeInfo, NULL, &pipelineLayout);
vkCmdPushConstants(cmd, pipelineLayout,
    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(vals), &vals);
```

Create the layout with the range from the start; retrofitting means destroy then recreate. Data copies immediately, so stack storage is fine. Pushes are command-buffer state - each buffer re-pushes - while writes persist in the set.

## Bindless: one array, indexed per draw

Without bindless, each material shape needs its own layout. Bindless binds one array once and lets each draw pick entries by index, so materials with different texture counts share pipelines. Indexing is core since Vulkan 1.2; query and keep a classic fallback:

```c
VkPhysicalDeviceDescriptorIndexingFeatures idx = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
VkPhysicalDeviceFeatures2 f2 = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &idx };
vkGetPhysicalDeviceFeatures2(physicalDevice, &f2);
bool bindlessSupported =
    idx.descriptorBindingPartiallyBound && idx.runtimeDescriptorArray;
```

`runtimeDescriptorArray` allows unsized arrays like `sampler2D globalTextures[]`; `descriptorBindingPartiallyBound` allows unwritten slots. Enable both at device creation with a separate features instance chained into device creation rather than reusing the queried one, which can enable unrequested features. Then build an update-after-bind pool, a layout with per-binding flags, and one set sized for the maximum:

```c
enum { kMaxBindlessResources = 1024 };
VkDescriptorPoolSize bpoolSizes[2] = {
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxBindlessResources },
    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxBindlessResources },
};
VkDescriptorPoolCreateInfo bpoolInfo = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
    .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = bpoolSizes,
};
vkCreateDescriptorPool(device, &bpoolInfo, NULL, &bindlessPool);

VkDescriptorSetLayoutBinding b[2] = { 0 };
b[0].binding = 10;
b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
b[0].descriptorCount = kMaxBindlessResources;
b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
b[1].binding = 11;
b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
b[1].descriptorCount = kMaxBindlessResources;
b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
VkDescriptorBindingFlags bf[2] = {
    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
};
VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
    .bindingCount = 2, .pBindingFlags = bf,
};
VkDescriptorSetLayoutCreateInfo blInfo = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .pNext = &flagsInfo,
    .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
    .bindingCount = 2, .pBindings = b,
};
vkCreateDescriptorSetLayout(device, &blInfo, NULL, &bindlessLayout);

VkDescriptorSetAllocateInfo ballocInfo = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = bindlessPool, .descriptorSetCount = 1,
    .pSetLayouts = &bindlessLayout,
};
vkAllocateDescriptorSets(device, &ballocInfo, &bindlessSet);
```

Binding 10 is the sampled-texture array; binding 11 mirrors it as storage images for fragment and compute writes. Size the pool for the maximum count per type, not the current one.

Pool flag, layout flag, and binding flags must agree or creation fails validation. On Vulkan 1.2 and later use the core names above; the `_EXT` aliases still work but are deprecated style. New textures queue at creation and flush before the next frame, one write per slot with a per-texture sampler or a default:

```c
uint32_t n = 0;
for (int i = pendingCount - 1; i >= 0; --i) {
    uint32_t slot = pending[i];
    VkDescriptorImageInfo *image = &imageInfos[n];
    image->sampler = tex[slot].sampler ? tex[slot].sampler : defaultSampler;
    image->imageView = tex[slot].view ? tex[slot].view : dummyTexture.view;
    image->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet *bw = &writes[n];
    *bw = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = bindlessSet, .dstBinding = 10, .dstArrayElement = slot,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = image,
    };
    pending[i] = pending[--pendingCount]; /* delete_swap */
    ++n;
}
vkUpdateDescriptorSets(device, n, writes, 0, NULL);
```

Walk the pending queue backwards so the swap-remove stays valid, fill the `imageInfos` and `writes` arrays as you go, then submit all N slots with one update call.

Slots left unwritten are legal, but still write a magenta dummy so a stale index shows magenta instead of garbage. For packed arrays add `VARIABLE_DESCRIPTOR_COUNT_BIT` with `VkDescriptorSetVariableDescriptorCountAllocateInfo`. The shader needs the qualifier and an unsized array:

```glsl
#extension GL_EXT_nonuniform_qualifier : enable
layout(set = 1, binding = 10) uniform sampler2D globalTextures[];
layout(set = 1, binding = 10) uniform sampler3D global_textures_3d[];
vec4 albedo = texture(globalTextures[nonuniformEXT(texIndex)], vTexcoord0);
```

`nonuniformEXT` tells the compiler neighbouring invocations may disagree; omitting it compiles but can sample the wrong texture under divergence. The 3D line aliases the same set and binding with a different view type, so 2D and 3D textures share one slot space.

## Reflection generates layouts

Hand-maintaining layouts across hundreds of variants fails; reflection from SPIR-V scales. Compile with `glslangValidator -V`, inspect with `spirv-dis`: set, binding, and offset decorations are the layout data. Khronos ships SPIRV-Reflect for production; the mechanism is a walk from magic `0x07230203`, opcode low and word count high:

```c
assert(data[0] == 0x07230203u);
size_t at = 5;
while (at < wordCount) {
    uint32_t op = data[at] & 0xFFFFu;
    uint16_t count = (uint16_t)(data[at] >> 16);
    /* EntryPoint, Decorate, Type*, Variable, ... */
    at += count;
}
```

`OpEntryPoint` maps to the stage; `OpDecorate` with `DescriptorSet` or `Binding` places the resource; `OpVariable` plus storage class selects resources, following pointer-to-concrete type to pick buffer versus sampler. Parse all stages into one map. Two limits: the binary cannot separate plain from dynamic buffers - annotate side-channel, e.g. a `dyn_` prefix - and each new resource kind needs another switch arm.

## Recap

- Layout describes bindings and immutable samplers; pipeline layout exposes sets plus push ranges with prefix compatibility.
- Pools allocate by type and count; writes rewire references and copies duplicate slots; `VK_WHOLE_SIZE` binds to buffer end; dynamic offsets arrive per bind.
- Per-frame motion rewrites mapped bytes, not the set; the depth remap keeps Chapter 10's `LESS_OR_EQUAL` valid; coherent memory skips the flush non-coherent memory needs.
- Push constants carry bytes via a range plus push; bindless is one array indexed per draw with dummy fallback and `nonuniformEXT`; pending slots flush in one batched update.
- SPIR-V reflection over entry points, decorations, and variables generates layouts.

## Exercises

1. **Dynamic offsets without extra sets.** Pack four MVPs in one `UNIFORM_BUFFER_DYNAMIC` buffer, write the set once with `VK_WHOLE_SIZE`, then draw four cubes by rebinding the same set with a different `pDynamicOffsets` entry. Confirm silence, pass one misaligned offset, and record the error.
2. **Bindless fallback drill.** Allocate a 1024-slot array with `PARTIALLY_BOUND`, write real views into slots 0-1 plus magenta dummies elsewhere, and render quads indexing slots 0, 1, and 7. Confirm the third renders magenta, drop `nonuniformEXT` under divergence, and note the failure. Carry the array into Chapter 14's uploads (Chapter 10's companion stays descriptor-free with an empty layout).
