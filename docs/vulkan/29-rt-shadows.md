# Chapter 29: Ray-Traced Shadows

> **Objectives:** Replace shadow maps with ray-queried visibility; trace one shadow ray per point light with correct `tMax`; derive per-light sample counts from four-frame variance; reproject history with depth-aware motion vectors; stabilize the result with temporal averaging plus a normal-rejecting spatial filter. Chapter 30 builds on the budgeted rays here for diffuse bounce, and Chapter 27 covers the TAA motion vectors that Section 29.3 adapts.
>
> **Flint note:** Flint keeps its raster shadow path for interactive CAD - depth maps are cheap and predictable for large assemblies. Use this chapter for reference shadows that validate the raster result, for clearance checks where contact precision matters, and for sun studies where cascade seams are unacceptable.

## 29.1 Why trace shadows instead of rendering maps

Shadow maps render the scene once per light, then compare depths at lighting time. That works, but resolution is fixed at capture time: distant receivers shimmer, thin geometry leaks, and sun lights need cascades with visible seams.

Ray-traced shadows ask the direct question instead: from this surface point, can the light be seen? No depth buffer, no cascade, no per-distance bias. The acceleration structures from the previous chapter already hold the scene, so each test is a short ray against the TLAS.

The tradeoff is cost per light. One ray per fragment per light is fine for a handful of point lights. Area lights and soft penumbrae need many rays each, which does not scale at a fixed maximum. The rest of this guide builds the adaptive version: spend rays where visibility is uncertain, reuse history where it is stable.

## 29.2 One ray per light with ray queries

The previous chapter used `vkCmdTraceRaysKHR` with a shader binding table. For shadows inside a lighting pass, ray queries are simpler: traverse the TLAS inline, with no SBT and no separate ray pipeline. Enable `VK_KHR_RAY_QUERY_EXTENSION_NAME` (`"VK_KHR_ray_query"`) - the bare `VK_KHR_ray_query` spelling is not a macro (case mismatch, won't compile) - and, in GLSL:

```glsl
#extension GL_EXT_ray_query : enable
layout(binding = 1) uniform accelerationStructureEXT tlas;
```

For each fragment, build the segment from surface to light:

```glsl
vec3 toLight = lightPos - worldPos;
float dist = length(toLight);
vec3 dir = toLight / dist;

rayQueryEXT q;
rayQueryInitializeEXT(q, tlas,
    gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
    0xFF, worldPos, 0.001, dir, dist);
rayQueryProceedEXT(q);

float shadow = 1.0;
if (rayQueryGetIntersectionTypeEXT(q, true)
        != gl_RayQueryCommittedIntersectionNoneEXT)
    shadow = 0.0;
```

Three details matter. `gl_RayFlagsTerminateOnFirstHitEXT` stops at the first occluder. The `0.001` `tMin` avoids self-intersection. The `tMax` of `dist` clips the ray at the light: without it, geometry behind the light would falsely shadow the fragment.[^tmax]

Loop this over lights and multiply each contribution by its `shadow`. That is the complete simple technique, and the right stopping point for point lights in small scenes.

> **Misconception: "More shadow rays always mean better shadows."**
> More rays only help where visibility is uncertain - penumbrae, thin occluders, grazing angles. Fully lit and fully shadowed regions converge after one ray; extra rays there burn time for an identical answer. The adaptive technique below exists to tell those cases apart.

## 29.3 Motion vectors that know when to quit

Spreading cost over frames only works if history can be trusted. Each frame computes a motion vector with a validity flag for the later passes.

Unlike the TAA vectors from Chapter 27, these compare clip-space depths to catch disocclusions:

```glsl
float depthDiff = abs(1.0 - prevNDC.z / currNDC.z);
float eps = 0.003 + 0.017 * abs(viewNormal.z);
vec2 motion = depthDiff < eps
    ? currNDC.xy - prevNDC.xy
    : vec2(-1.0, -1.0);
```

`eps` widens for grazing normals, where small camera moves cause large depth swings. The sentinel `(-1, -1)` means reprojection failed: discard history and take the maximum sample count. Every later pass keys off that sentinel.

## 29.4 Variance tells each pixel its ray budget

History lives in 3D textures - width by height by light index - so one dispatch covers every pair:

```c
vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, activeLightCount);
```

Here `px = ivec2(gl_GlobalInvocationID.xy)` is the fragment coordinate and `light = gl_GlobalInvocationID.z` is the light layer, so `ivec3(px, light)` addresses one fragment-light texel.

The first compute pass measures instability. Each texel holds the last four visibility values for its light; the pass outputs `max - min`:

```glsl
vec4 h = texelFetch(visibilityHistory, ivec3(px, light), 0);
float var = max(max(h.x, h.y), max(h.z, h.w))
          - min(min(h.x, h.y), min(h.z, h.w));
imageStore(varianceImg, ivec3(px, light), vec4(var));
```

History starts cleared.

The second pass converts variance into a sample count. It loads an 8x8 tile plus a 2-texel halo into LDS (shared memory) before the `barrier()` that makes neighbors visible to the whole workgroup - the same halo pattern as the VRS pass - takes a 5x5 maximum to dilate hot spots, then smooths with a 13x13 tent filter. That spatial result is averaged 50/50 with the mean of the cached variation, giving one stable `filtered` value per fragment-light pair.

The budget update is a small state machine:

```glsl
uint count = MAX_SHADOW_SAMPLES; // cold start or reprojection failed
if (motion.x != -1.0) {
    count = countHistory.x;
    bool stable = countHistory.x == countHistory.y
               && countHistory.x == countHistory.z
               && countHistory.x == countHistory.w;
    if (filtered > 0.2 && count < MAX_SHADOW_SAMPLES)
        count += 1;
    else if (stable && count > 0)
        count -= 1;
    if (count == 0 && all(lessThan(countHistory, uvec4(1))))
        count = 1; // a stable scene still needs one ray
}
```

High variance ramps up one ray at a time; four frames of agreement ramps down. The floor of one prevents a static camera from going dark. Fragments that see the light (`light_active`) need more samples than confirmed-shadowed ones: a visible light contributes direct radiance so its noise is visible, while a fully occluded light contributes zero either way. Shift both history caches forward each frame:

```glsl
varCache[3] = varCache[2];
varCache[2] = varCache[1];
varCache[1] = varCache[0];
varCache[0] = filtered;
```

## 29.5 Tracing the budgeted rays and filtering the noise

Visibility tracing runs only where the budget is nonzero, skipping lights facing away or out of range:

```glsl
float NoL = clamp(dot(normal, dir), 0.0, 1.0);
float atten = attenuationSquareFalloff(toLight, 1.0 / light.radius);
if (NoL <= 0.001 || dist > light.radius || atten <= 0.001)
    return 0.0;
```

Each sample jitters the direction with a Poisson-disk offset indexed by sample and frame, so the pattern decorrelates over time. `POISSON` holds `MAX_SHADOW_SAMPLES * FRAME_HISTORY` 2D disk samples, so index `s * FRAME_HISTORY + frameIndex % FRAME_HISTORY` gives sample `s` a different disk point each frame. The tangent frame spreads the offset perpendicular to the ray, and `k = dist * 0.01` scales penumbra spread with distance:

```glsl
vec3 tangentX = normalize(cross(dir, abs(dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
vec3 tangentY = cross(dir, tangentX);
float k = dist * 0.01; // wider cone for distant lights, tighter near the surface
float visibility = 0.0;
for (uint s = 0; s < count; ++s) {
    vec2 p = POISSON[s * FRAME_HISTORY + frameIndex % FRAME_HISTORY];
    vec3 j = normalize(dir + tangentX * p.x * k + tangentY * p.y * k);

    rayQueryEXT q;
    rayQueryInitializeEXT(q, tlas,
        gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
        0xFF, worldPos, 0.001, j, dist);
    rayQueryProceedEXT(q);
    if (rayQueryGetIntersectionTypeEXT(q, true)
            == gl_RayQueryCommittedIntersectionNoneEXT)
        visibility += 1.0;
}
return visibility / float(count);
```

Push the result into the visibility history (append on success, overwrite all four slots on failure), then run the final filter. Temporally, average the four cached values. Spatially, apply a fixed 5x5 Gaussian, skipping neighbors whose normals diverge:

```glsl
if (dot(centerNormal, sampleNormal) <= 0.9)
    continue; // prevents leaking light across edges
accum += temporalValue * gauss[y + 2][x + 2];
```

The lighting pass then reads one filtered texel per fragment-light pair (`screenPx = ivec2(gl_FragCoord.xy)` is the fragment coordinate, `lightIdx` the light layer):

```glsl
float shadow = texelFetch(shadowTex, ivec3(screenPx, lightIdx), 0).r;
float atten = attenuationSquareFalloff(toLight, 1.0 / light.radius) * shadow;
```

That texture decouples the shadow system from the renderer: shadow maps can stay for distant lights while rays handle hero lights.

[^tmax]: An explicit beyond-light TEXT (hit-distance) check is redundant here: with `tMax = dist`, any committed hit is already in front of the light.

## Recap

- Ray queries test surface-to-light segments inline with no SBT; clamp `tMax` to the light distance and offset `tMin` against self-hits.
- One ray per light suits point lights; soft shadows need adaptive budgets driven by history, not a global count.
- Depth-aware motion vectors with a `(-1, -1)` sentinel gate all reuse; failure resets to maximum samples.
- Variance (max minus min over four frames), dilated by a 5x5 max and smoothed by a tent plus temporal blend, sets the per-fragment per-light ray count.
- Poisson-jittered rays converge over frames; a temporal average plus a normal-rejecting Gaussian removes the remaining noise before lighting.

## Exercises

1. **Cold-start audit.** Clear all history, render one frame of a two-light scene, and dump the sample-count layer per light. Confirm every texel reads `MAX_SHADOW_SAMPLES`, then step four frames with a static camera and record how the count decays in lit versus penumbra regions.
2. **Normal-rejection break test.** Set the `0.9` dot threshold to `-1.0` (accept all neighbors) and capture a thin-occluder edge. Describe where light leaks, then restore the threshold and confirm the leak closes without raising the budget.
