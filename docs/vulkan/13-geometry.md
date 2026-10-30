# Chapter 13: Geometry Processing

> **Objectives:** Describe vertex fetching; configure tessellation control and evaluation shaders; use geometry shaders; capture output with transform feedback; distinguish culling from clipping; trace a vertex from object space through perspective divide to the viewport.

Draws from Chapter 12 supply vertices and indices; this chapter traces what the pipeline does with them before rasterization. The vocabulary matters everywhere: fetching, subdivision, per-primitive work, capture, culling, clipping, and the transform to pixels. Chapter 14 continues into rasterization, fragments, and blending.

> **Flint note:** Flint draws flat 2D interface geometry, so most of this chapter is background. Vertex input, culling state, scissor, and viewport setup apply directly; tessellation, geometry shaders, and transform feedback stay disabled.

## 13.1 Vertex fetching starts the pipeline

> **Out-of-chapter context:** vertex fetching belongs to pipeline vertex-input setup, not the source geometry-processing chapter; kept here as background for the vertex journey below.

Every draw begins by pulling bytes out of vertex buffers. Vulkan makes that mapping explicit in `VkPipelineVertexInputStateCreateInfo`, which holds two arrays: bindings describe *how* to step through a buffer, attributes describe *what* each field means to the shader.

```c
VkVertexInputBindingDescription binding = {
    .binding = 0,
    .stride = sizeof(Vertex),           /* bytes between vertices */
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

VkVertexInputAttributeDescription attrs[2] = {
    { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
      .offset = offsetof(Vertex, pos) },
    { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .offset = offsetof(Vertex, color) },
};

VkPipelineVertexInputStateCreateInfo vertexInput = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = 1,
    .pVertexBindingDescriptions = &binding,
    .vertexAttributeDescriptionCount = 2,
    .pVertexAttributeDescriptions = attrs,
};
```

`stride` plus `offset` must stay inside the bound buffer, and `format` controls both size and numeric conversion. `VK_VERTEX_INPUT_RATE_VERTEX` advances once per vertex; `VK_VERTEX_INPUT_RATE_INSTANCE` advances once per instance for per-instance transforms or colors. The vertex shader then sees `layout(location = 0) in vec2 pos;` with matching locations. A mismatch here reads garbage rather than raising an error, so check locations, offsets, and formats together.

## 13.2 Tessellation subdivides patches

Tessellation sits between the vertex shader and everything downstream. It takes a *patch*, a group of `patchControlPoints` vertices, and subdivides it into points, lines, or triangles. It is optional: query `VkPhysicalDeviceFeatures::tessellationShader` before using it.

Pipeline state contributes only one number:

```c
VkPipelineTessellationStateCreateInfo tess = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
    .patchControlPoints = 4,   /* vertices per patch; maxTessellationPatchSize >= 32 guaranteed */
};
```

Everything else lives in two shaders. The tessellation control shader (TCS) runs once per output control point, sees the whole input patch as `gl_in[]`, and writes the subdivision factors `gl_TessLevelInner` and `gl_TessLevelOuter`. The fixed-function tessellator consumes those factors and emits barycentric coordinates. The tessellation evaluation shader (TES) runs once per generated vertex, reads `gl_TessCoord`, and computes its position, usually by interpolating control points and displacing along a normal:

```glsl
#version 450 core
layout(quads, fractional_odd_spacing, ccw) in;
layout(set = 0, binding = 0) uniform sampler2D texDisp;
layout(push_constant) uniform PC { mat4 mvp; float scale; } pc;
void main(void)
{
    vec4 a = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_TessCoord.x);
    vec4 b = mix(gl_in[2].gl_Position, gl_in[3].gl_Position, gl_TessCoord.x);
    vec4 pos = mix(a, b, gl_TessCoord.y);
    float d = texture(texDisp, gl_TessCoord.xy).x;
    pos.z = d * pc.scale;
    gl_Position = pc.mvp * pos;
}
```

The `layout(quads, fractional_odd_spacing, ccw) in;` line selects quad subdivision, smooth fractional spacing, and counter-clockwise winding; each maps to a SPIR-V `OpExecutionMode`. Outer levels tessellate patch edges, so adjacent patches sharing the same outer level join without cracks, while inner levels fill the interior. A TCS that writes `0.0` or NaN to any level discards the whole patch, which is the cheap way to cull off-screen patches before subdivision work happens.

The primitive mode selects the domain: `triangles` and `quads` subdivide surfaces, while `isolines` emits line strips for hair, wire, or contour rendering. Adding `point_mode` switches triangles and quads to emit points instead of filled primitives, useful for particle or point-sampled displacement. The spacing mode controls how integer levels step to fractional ones: `equal_spacing` snaps levels up to integers and subdivides uniformly, while `fractional_even_spacing` and `fractional_odd_spacing` interpolate edge lengths smoothly as levels change, avoiding popping during LOD transitions. The winding selector (`cw` or `ccw`) sets output orientation, which must agree with rasterization `frontFace` or culled faces invert.

> **Limits box:** tessellation I/O (`maxTessellationControlPerVertexInputComponents`, `maxTessellationControlPerVertexOutputComponents`, `maxTessellationControlPerPatchOutputComponents`, `maxTessellationControlTotalOutputComponents`, `maxTessellationEvaluationInputComponents`, `maxTessellationEvaluationOutputComponents`); geometry (`maxGeometryShaderInvocations`, `maxGeometryInputComponents`, `maxGeometryOutputComponents`, `maxGeometryOutputVertices`, `maxGeometryTotalOutputComponents`); clip/cull (`maxClipDistances`, `maxCullDistances`, `maxCombinedClipAndCullDistances`); points/lines (`pointSizeRange` + `pointSizeGranularity`, `lineWidthRange` + `lineWidthGranularity`); viewports (`maxViewports`, `maxViewportDimensions`, `viewportBoundsRange`, `viewportSubPixelBits`).

Per-patch data uses `patch out` variables with a `barrier()` call so invocations synchronize before reading each other's writes. Check `maxTessellationPatchSize` (>= 32 guaranteed) and `maxTessellationGenerationLevel` (>= 64 guaranteed) in `VkPhysicalDeviceLimits`. Note `gl_in.length()` in the TES equals the TCS output-vertex count, which must match `patchControlPoints`.

> **Misconception:** "Tessellation creates detail for free."
> It creates triangles for free; detail still costs vertex processing, rasterization, and bandwidth downstream. A level of 64 on a full-screen quad emits tens of thousands of triangles every frame. Scale levels by distance, split large surfaces into smaller patches, and prefer a texture for fine detail that shading alone can fake.

## 13.3 Geometry shaders see whole primitives

The geometry shader (GS) is the only stage that receives an entire primitive, including adjacency variants, and the only stage that can delete vertices and emit new ones. Like tessellation, it is optional: query it with `vkGetPhysicalDeviceFeatures` (`VkPhysicalDeviceFeatures::geometryShader`), then enable it by setting `geometryShader = VK_TRUE` in `VkDeviceCreateInfo` (`pEnabledFeatures`) at device creation.

Its contract is declared up front:

```glsl
#version 450 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;
void main(void)
{
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
}
```

Input layouts are `points`, `lines`, `triangles`, or the adjacency forms; outputs are `points`, `line_strip`, or `triangle_strip`. `EmitVertex()` pushes the current outputs down the pipeline, and `EndPrimitive()` cuts the strip so the next vertex starts a fresh primitive. `max_vertices` maps to SPIR-V `OpExecutionMode OutputVertices` and must fit `maxGeometryOutputVertices` (at least 256).

A GS can also run instanced via `layout(invocations = 4) in;`, with `gl_InvocationID` selecting per-copy behavior such as a different transform or `gl_ViewportIndex`. Because amplification multiplies downstream work, reserve geometry shaders for per-primitive jobs: billboard expansion, point sprites, or viewport broadcast.

## 13.4 Transform feedback captures what shaders produced

> **Out-of-chapter context:** transform feedback comes from `VK_EXT_transform_feedback`, not the source geometry-processing chapter; kept here as background. Flint leaves it disabled.

Sometimes the result of geometry processing is itself the asset: a simulated particle buffer or skinning output consumed by a later dispatch. Transform feedback captures vertex, tessellation-evaluation, or geometry-shader outputs into buffers instead of discarding them after rasterization.

In Vulkan this comes from the `VK_EXT_transform_feedback` extension, not core. Enable it by chaining `VkPhysicalDeviceTransformFeedbackFeaturesEXT` (`transformFeedback = VK_TRUE`) through `pNext` into `VkDeviceCreateInfo` at device creation, declare `layout(xfb_buffer = 0, xfb_stride = 16) out vec4 savedPos;` in GLSL, bind capture buffers with `vkCmdBindTransformFeedbackBuffersEXT`, then bracket the draw with `vkCmdBeginTransformFeedbackEXT` / `vkCmdEndTransformFeedbackEXT`. Both begin and end take counter-buffer arguments (`firstCounterBuffer`, `counterBufferCount`, `pCounterBuffers`, `pCounterBufferOffsets`); pass `counterBufferCount = 0` with null pointers when byte-continue counters are not needed. Respect `VkPhysicalDeviceTransformFeedbackPropertiesEXT` limits: `maxTransformFeedbackStreams`, `maxTransformFeedbackBuffers`, `maxTransformFeedbackBufferSize`, `maxTransformFeedbackStreamDataSize`, `maxTransformFeedbackBufferDataSize`, and `maxTransformFeedbackBufferDataStride`. Captured data can then be re-bound as vertex input without a CPU round trip. Flint does not use this path; interface vertex buffers are rebuilt directly on the CPU side.

## 13.5 Culling discards, clipping trims

Culling removes whole primitives; clipping trims primitives that cross a boundary. Both start with `VkPipelineRasterizationStateCreateInfo`: `cullMode` (`VK_CULL_MODE_NONE`, `VK_CULL_MODE_FRONT_BIT`, `VK_CULL_MODE_BACK_BIT`, `VK_CULL_MODE_FRONT_AND_BACK`) and `frontFace` (`VK_FRONT_FACE_COUNTER_CLOCKWISE`, `VK_FRONT_FACE_CLOCKWISE`) decide back-face culling from winding order after projection. Getting the winding wrong makes an object vanish or render inside-out.

User clipping and culling add shader-driven planes. Decorate a float array output with `ClipDistance` or `CullDistance` (GLSL `gl_ClipDistance[i]` and `gl_CullDistance[i]`), which map to SPIR-V `OpCapability ClipDistance` / `OpCapability CullDistance`. Enable `VkPhysicalDeviceFeatures::shaderClipDistance` / `shaderCullDistance` (queried with `vkGetPhysicalDeviceFeatures`, enabled via `VkDeviceCreateInfo`), and respect `maxClipDistances`, `maxCullDistances`, and the combined `maxCombinedClipAndCullDistances` in `VkPhysicalDeviceLimits`. Positive means inside. The difference: clip distances interpolate across the primitive and the clipper trims edges, producing new vertices along the zero crossing; cull distances discard the entire primitive if *any* vertex is negative.

Viewport scissoring remains the blunt 2D clip Flint relies on for panels and scroll views; clip planes are the 3D analogue for section views.

## 13.6 Coordinate spaces and the perspective divide

A vertex travels through a chain of spaces, and each step has one job:

```
object -> world -> view -> clip -> NDC -> framebuffer
  model    model    view    proj    divide    viewport
  matrix   matrix   matrix  matrix   by w      + depth
```

The application supplies model, view, and projection matrices, usually combined into a single MVP multiplied in the last geometry stage (`gl_Position = mvp * pos;`). `gl_Position` is in *clip space*, a homogeneous `{x, y, z, w}` coordinate. The hardware then performs the perspective divide, `{x/w, y/w, z/w}`, yielding normalized device coordinates (NDC). That division is what makes distant objects smaller: `w` carries view-space depth, so dividing by it compresses far geometry toward the center.

NDC maps to pixels through `VkViewport`:

```c
VkViewport vp = {
    .x = 0.0f, .y = 0.0f,
    .width = (float)width, .height = (float)height,
    .minDepth = 0.0f, .maxDepth = 1.0f,
};
vkCmdSetViewport(cmd, 0, 1, &vp);
```

Framebuffer `x = (ndc.x + 1) * width / 2 + vp.x`, similarly for `y`, and depth maps from NDC `z` into `[minDepth, maxDepth]`. Multiple viewports (up to `maxViewports`; at least 16 when `multiViewport` is supported, else `maxViewports == 1` and `viewportCount` must be 1 with `gl_ViewportIndex` fixed at 0) are selected per-primitive by `gl_ViewportIndex` from a geometry shader; without one, viewport zero is used. `gl_PointSize` and `lineWidth` add state on top, each clamped to device ranges.

## Recap

- Vertex fetching pairs bindings (stride, rate) with attributes (location, format, offset); instance rate feeds per-instance data.
- Tessellation subdivides patches via TCS inner/outer levels; TES places vertices from `gl_TessCoord`. Zero/NaN discards the patch.
- Geometry shaders process whole primitives, emit with `EmitVertex`, cut strips with `EndPrimitive`, and optionally instance with `gl_InvocationID`.
- Transform feedback via `VK_EXT_transform_feedback` captures shader outputs into buffers for reuse without CPU readback.
- Back-face culling drops primitives by winding; `ClipDistance` trims edges while `CullDistance` drops the primitive if any vertex is negative.
- Clip-space `gl_Position` divides by `w` into NDC, then `VkViewport` scales and biases into framebuffer coordinates and depth range.

## Exercises

1. **Cracked versus seamless patches.** Build a two-patch quad grid with a TCS that assigns different outer levels to the shared edge. Render in wireframe (`VK_POLYGON_MODE_LINE`), observe the crack, then equalize the shared outer level and confirm the seam closes. Explain why only outer levels affect edge continuity.
2. **Clip versus cull.** Write a vertex shader that outputs one `gl_ClipDistance` plane and one `gl_CullDistance` plane from push-constant distances. Move a triangle so one vertex goes negative on each plane in turn, capture the result, and state which case trimmed the triangle and which case removed it entirely.
