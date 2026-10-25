# Chapter 10: Graphics Pipelines

> **Objectives:**
> - Trace the graphics pipeline from `vkCmdDraw` through blending.
> - Describe vertex bindings, attributes, and input-assembly topology.
> - Declare dynamic viewport and scissor with matching counts.
> - Configure rasterization, multisampling, depth/stencil, and blending with explicit values.
> - Create a `VkPipeline` with layout, cache, render pass, and derivation controls.

Chapters 8 and 9 gave you shader modules with a cache and pipeline layouts for resources. This chapter freezes everything into one immutable object. Chapter 11 builds the render pass and framebuffers, and Chapter 12 records the commands that bind it and draw.

## 1. The pipeline as a production line

A graphics pipeline is a fixed sequence of stages. Commands enter at `vkCmdDraw` and fragments leave as pixels in a framebuffer attachment:

```c
void vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount,
    uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
```

For a plain draw, set `instanceCount` to 1 and both `first` values to 0. A pipeline must be bound first; drawing without one is undefined.

In order, the stages are input assembly, vertex shader (the only mandatory stage), optional tessellation stages, optional geometry shader, primitive assembly with clipping and culling, rasterization, early fragment tests, fragment shader, then late tests and blending. The compute pipeline from Chapter 8 is the contrast: one compute shader plus a layout, with no render pass.

Baking all of this into one object lets the driver optimize across shader and fixed-function state at creation time. That is why creation takes stages, eight state structs, a layout, and a subpass, and why nothing changes afterward except listed dynamic state.

## 2. Layout and cache arrive ready

Two helpers arrive as ready handles. The pipeline layout from Chapter 9 lists set layouts plus push-constant ranges; treat it here as the argument signature. The companion triangle needs no descriptors, so its layout is empty:

```c
VkPipelineLayoutCreateInfo emptyInfo = { 0 };
emptyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
emptyInfo.setLayoutCount = 0;
emptyInfo.pSetLayouts = NULL;
emptyInfo.pushConstantRangeCount = 0;
emptyInfo.pPushConstantRanges = NULL;
vkCreatePipelineLayout(device, &emptyInfo, NULL, &pipelineLayout);
```

The pipeline cache from Chapter 8 pools compilation results. Pass the same `VkPipelineCache` to every create call and the driver reuses a near match, within one run and, via bytes checked against `vendorID`, `deviceID`, and `pipelineCacheUUID`, across runs. `VK_NULL_HANDLE` is valid but skips that reuse. Give each thread its own cache when building in parallel and merge with `vkMergePipelineCaches` before the single save.

> **Flint note:** Flint bakes one opaque pipeline at startup (triangle list, fill, back-cull, depth `LESS_OR_EQUAL`, no blending, one sample) and reuses it for every surface. It keeps one `VkPipelineCache`, saves its bytes, and pre-seeds the next launch; a second pipeline appears only for premultiplied-alpha UI blending.

## 3. Vertex input and input assembly

Vertex input has two halves. A **binding** describes an array of structures: index, byte stride, and per-vertex or per-instance stepping. An **attribute** describes one field: shader `location`, source binding, `VkFormat`, and byte offset:

```c
typedef struct Vertex { float pos[2]; float uv[2]; float color[4]; } Vertex;
static const VkVertexInputBindingDescription binding =
    { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
    { 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos) },
    { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv) },
    { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color) },
};
static const VkPipelineVertexInputStateCreateInfo vertexInput = {
    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    NULL, 0, 1, &binding, 3, attributes
};
```

The shader must agree:

```glsl
layout (location = 0) in vec2 i_pos;
layout (location = 1) in vec2 i_uv;
layout (location = 2) in vec4 i_color;
```

Indices and locations need not be contiguous, but every location the shader uses must be described. Source buffers need `VK_BUFFER_USAGE_VERTEX_BUFFER_BIT`.

Input assembly groups those vertices into primitives:

```c
static const VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE
};
```

`POINT_LIST`, `LINE_LIST`, and `TRIANGLE_LIST` build independent primitives, while strips and fans share vertices. Topologies ending in `WITH_ADJACENCY` supply neighbor vertices to the geometry shader and are invalid without an active geometry stage. `PATCH_LIST` needs `VkPipelineTessellationStateCreateInfo` with a `patchControlPoints` count. Enable `primitiveRestartEnable` only for indexed strips or fans, where `0xFFFF` or `0xFFFFFFFF` cuts one strip and starts the next; lists ignore it.

> **Misconception:**
>
> "The pipeline is a suggestion; I can tweak state after creation." No. A `VkPipeline` is baked. Topology, cull mode, and blend factors become immutable at creation, so changing fill mode or topology means a new pipeline, ideally via the cache. Only states listed in `pDynamicState` may change with `vkCmdSet*`, and leaving a dynamic state unset after binding leaves it undefined rather than a default.

## 4. Viewport, scissor, and dynamic state

The viewport maps normalized coordinates to pixels; the scissor clips to a rectangle:

```c
VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
VkRect2D scissor = { { 0, 0 }, { width, height } };
vkCmdSetViewport(cmd, 0, 1, &viewport);
vkCmdSetScissor(cmd, 0, 1, &scissor);
```

Window sizes change, so keep both dynamic. Declare counts up front with NULL pointers; values come later with `vkCmdSet*`:

```c
static const VkPipelineViewportStateCreateInfo viewportState = {
    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    NULL, 0, 1, NULL, 1, NULL
};
static const VkDynamicState dynamicStates[] =
    { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
static const VkPipelineDynamicStateCreateInfo dynamic = {
    VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    NULL, 0, 2, dynamicStates
};
```

Three validity rules govern this pair. First, `viewportCount` must equal `scissorCount`; with both dynamic the pointers stay NULL and the counts reserve the slots. Second, a state listed in `pDynamicState` is excluded from baking and must be set after `vkCmdBindPipeline` and before use: bind, set, then draw. Third, rebinding can reset dynamic state, so set it again after every bind. The core dynamic list is viewport, scissor, line width, depth bias, blend constants, depth bounds, and stencil masks plus reference. Omitting `VK_DYNAMIC_STATE_VIEWPORT` while calling `vkCmdSetViewport` is a validation error: counts live in the viewport struct, values live in the command.

## 5. Rasterization, multisampling, and depth/stencil

Rasterization turns primitives into fragments and controls culling and fill:

```c
static const VkPipelineRasterizationStateCreateInfo raster = {
    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL,
    VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE,
    VK_FALSE, 0.0f, 0.0f, 0.0f, 1.0f
};
```

`FILL` draws solid triangles; `LINE` and `POINT` give wireframe and point views. `frontFace` is `CLOCKWISE` to match the companion triangle winding; either winding is valid only when it matches the submitted triangle winding, otherwise back-face culling removes the visible face. Keep `depthClampEnable` at `VK_FALSE` unless the `depthClamp` feature is enabled, and keep `lineWidth` at 1.0 unless `lineWidthRange` confirms wider lines.

A 2D UI pipeline disables multisampling and depth explicitly; every unused feature still needs an explicit disabled struct:

```c
static const VkPipelineMultisampleStateCreateInfo multisample = {
    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    NULL, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0.0f,
    NULL, VK_FALSE, VK_FALSE
};
static const VkPipelineDepthStencilStateCreateInfo depthStencil = {
    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    NULL, 0, VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS,
    VK_FALSE, VK_FALSE,
    {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0, 0, 0},
    {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0, 0, 0},
    0.0f, 1.0f
};
```

The opaque companion instead enables depth with both enables true and `depthCompareOp` of `VK_COMPARE_OP_LESS_OR_EQUAL`, keeping stencil parked at `KEEP` with `ALWAYS` and zero masks and bounds at `0.0f` and `1.0f`. `blendConstants {0.0f, 0.0f, 0.0f, 0.0f}`, depth bounds `0.0f`/`1.0f`, and `minSampleShading 0.0f` are inert while `blendEnable`, `depthBoundsTestEnable`, and `sampleShadingEnable` stay `VK_FALSE`. Enable depth testing only when a depth attachment exists. When bounds testing is enabled the range becomes dynamic with `VK_DYNAMIC_STATE_DEPTH_BOUNDS` via `vkCmdSetDepthBounds`, which needs the `depthBounds` feature. If resolving, pair each multisample color attachment (`DONT_CARE`) with a single-sample resolve attachment (`STORE`); with one sample the resolve reference stays NULL.

## 6. Color blending

Blending combines fragment output with the attachment. Each attachment gets its own state; the logic operation is shared.

```c
static const VkPipelineColorBlendAttachmentState opaqueAttachment = {
    VK_FALSE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO,
    VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
};
```

UI blends with premultiplied alpha, the choice when the shader already multiplied color by alpha:

```c
static const VkPipelineColorBlendAttachmentState blendAttachment = {
    VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
};
static const VkPipelineColorBlendStateCreateInfo blending = {
    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY,
    1, &blendAttachment, { 0.0f, 0.0f, 0.0f, 0.0f }
};
```

Set `blendEnable` to `VK_FALSE` for opaque passes; `colorWriteMask` still applies.

## 7. Creating the pipeline

A pipeline renders inside a render pass, so the render pass from Chapter 11 must exist first. Compatibility is narrow: a pipeline built for pass `R` and subpass `i` works with a framebuffer for `R'` only when formats, sample counts, and roles match. Size may differ.

Gather the Chapter 8 stages, all state structs above, the layout, and the render pass into one create info:

```c
VkPipelineShaderStageCreateInfo stages[2] = {
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vertModule, "main", NULL },
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragModule, "main", NULL }
};
VkGraphicsPipelineCreateInfo pipelineInfo = {
    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0,
    2, stages, &vertexInput, &inputAssembly, NULL,
    &viewportState, &raster, &multisample,
    &depthStencil, &blending, &dynamic,
    pipelineLayout, renderPass, 0, VK_NULL_HANDLE, -1
};
VkPipeline pipeline = VK_NULL_HANDLE;
vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineInfo, NULL, &pipeline);
```

`pTessellationState` is NULL with no tessellation shader. `pipelineCache` is the Chapter 8 handle, `pipelineLayout` the Chapter 9 handle, and `viewportState` is the struct from section 4, so this snippet compiles once the surrounding handles exist. Flags select derivation: `ALLOW_DERIVATIVES` marks a parent, `DERIVATIVE` builds a child, and `DISABLE_OPTIMIZATION` skips optimization. Derivation uses exactly one mechanism: `basePipelineHandle` to a parent with the allow flag, or `basePipelineIndex` to a sibling in the same call, never both; leave them `VK_NULL_HANDLE` and -1 when unused.

The device owns the pipeline, so `vkDestroyPipeline` must run after the pipeline leaves all queues and command buffers and before device destruction; layout, cache, and render pass have independent lifetimes once creation returns. Recording is then bind, set dynamic state, draw. Chapter 11 details the pass and framebuffer, and Chapter 12 walks the full frame from acquire through present.

---

### Recap

- The pipeline flows from input assembly through vertex processing, optional tessellation and geometry stages, rasterization, fragment shading, and blending. Only the vertex shader is mandatory.
- Layout (Chapter 9) and cache (Chapter 8) arrive as handles; the cache amortizes compilation and the layout fixes the resource signature.
- Vertex input splits into bindings (stride, step rate) and attributes (location, format, offset); the shader must declare matching locations.
- Input assembly picks topology; adjacency needs a geometry shader, patches need tessellation, restart applies to indexed strips and fans.
- Viewport and scissor counts must match; dynamic state is set after every bind and unset dynamic state is undefined.
- Rasterization sets fill, culling, and winding; multisampling, depth/stencil, and blending each need explicit disabled state even when unused, with resolve pairing multisample `DONT_CARE` to single-sample `STORE`.
- Pipelines are created against a render pass and subpass; derivation uses exactly one of handle or index.

### Exercises

1. **Build the Flint 2D pipeline.** Create a pipeline with `TRIANGLE_LIST`, dynamic viewport and scissor, back-face culling, one sample, depth off, and premultiplied-alpha blending. Record begin, bind, set viewport and scissor, bind vertex buffers, and draw; then resize and confirm only the `vkCmdSet*` arguments change.
2. **Break compatibility and derivation.** Duplicate the render pass with `R8G8B8A8_UNORM` instead of `B8G8R8A8_SRGB` and note the validation error. Then create two pipelines in one call with the second using `basePipelineIndex = 0` plus the derivative flag, and set both handle and index to record which message fires.
