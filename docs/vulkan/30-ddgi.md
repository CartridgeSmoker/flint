# Chapter 30: Diffuse Global Illumination

> **Objectives:** Distinguish direct from indirect lighting; describe the DDGI probe grid with irradiance and visibility volumes; trace per-probe rays storing radiance plus distance; offset probes out of geometry; update probes with hysteresis and filter borders; sample eight neighbours with a visibility weight; add the bounce to `calculate_lighting`. Chapter 31 continues with ray-traced specular reflections, and Chapter 23 covers the deferred lighting loop that Section 30.7 extends.
>
> **Flint note:** Flint interiors - rooms, cabinets, machine bays - gain legibility from bounced light. Price it honestly: a probe volume plus rays every frame is heavy for a CAD viewport. Start with a small volume over the work area, quarter-resolution sampling, and few rays per probe; freeze offsets for static models.

## 30.1 Direct light stops at the first hit

Direct lighting shades what the G-buffer sees and stops. It misses what makes a room feel lit: green curtains tinting the nearby floor, sky colour washing down a corridor, arches darkening where geometry blocks the bounce.

Indirect lighting is the rest of the journey. Light hits a hidden surface, bounces onto a visible one, then reaches the camera. Rasterization cannot see those hidden surfaces, so it cannot compute the bounce. Ray tracing can: fire a ray from anywhere, find the hit, shade it with the live lights.

Tracing every pixel every frame is still too costly. DDGI changes the rate: trace from a sparse probe grid, cache the result, and let pixels interpolate. The grid is far coarser than the screen, so only soft diffuse bounces survive - exactly what indirect diffuse needs.

## 30.2 Probes, irradiance, and visibility

A light probe records incoming light in all directions. An irradiance volume is a regular 3D grid of probes with fixed spacing. Regularity matters: any pixel finds its eight neighbours by index arithmetic, and trilinear weights follow from its position in the cell.

Each probe stores two atlases. Irradiance holds diffuse colour per direction. Visibility holds mean distance and mean squared distance to nearby geometry per direction, used later to suppress leaks through walls. Both use octahedral mapping, which unwraps sphere directions into square texels, plus a one-texel border around each probe cell so bilinear taps interpolate correctly.

The pipeline has five stages:

1. Trace a few rays per probe; store radiance in RGB, hit distance in alpha.
2. Blend fresh radiance into the irradiance atlas with hysteresis.
3. Blend fresh distances into the visibility atlas the same way.
4. Nudge probes out of geometry (offsetting, run briefly at setup).
5. Sample the eight nearest probes per pixel, weighted by visibility.

> **Misconception: "DDGI adds mirror reflections and sharp shadows."**
> No. Each probe holds a heavily filtered directional average, which preserves colour bleeding and soft occlusion but destroys specular detail. DDGI feeds the diffuse bounce term only; reflections and sun shadows stay in their own passes.

## 30.3 One row of rays per probe

The ray pass writes a texture where each row is one probe and each column is one ray: 128 rays over 24 probes gives a 128x24 image. RGB carries shaded radiance, alpha carries distance for the offset and visibility stages.

```glsl
#extension GL_EXT_ray_tracing : enable
struct RayPayload { vec3 radiance; float distance; };
```

Raygen maps launch index to probe and ray, picks a rotated spherical-Fibonacci direction so frames cover the sphere differently, and traces:

```glsl
layout(location = 0) rayPayloadEXT RayPayload payload;
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, rgba32f) uniform image2D radianceOut;

void main() {
    ivec2 coord = ivec2(gl_LaunchIDEXT.xy);
    ivec3 cell = probeIndexToGridIndices(coord.y);
    vec3 origin = gridIndicesToWorld(cell, coord.y);
    vec3 dir = normalize(randomRotation
        * sphericalFibonacci(coord.x, PROBE_RAYS));
    payload.radiance = vec3(0.0);
    payload.distance = 0.0;
    traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0,
        origin, 0.0, dir, 100.0, 0);
    imageStore(radianceOut, coord, vec4(payload.radiance, payload.distance));
}
```

Rotation plus the hysteresis blend in section 30.5 lets a few rays per frame converge, as with TAA jitter. Hit shades the remote point with direct lights plus an optional prior-frame irradiance fetch for a cheap second bounce; backfaces are flagged with negative distance scaled by -0.2, that is distance = (tmin+t) * -0.2. The sign is the downstream signal: the offset stage (section 30.4) counts negatives to detect buried probes, while the update shaders (section 30.5) skip negative distances so backfaces never pollute irradiance or visibility. Miss returns sky:

```glsl
// closest hit
vec3 pos, normal;
vec3 albedo = shadeHitPoint(pos, normal);
if (gl_HitKindEXT == gl_HitKindBackFacingTriangleEXT) {
    // Inside-geometry ray: negative distance, damped by 0.2 so one far
    // backface does not dominate the offset direction.
    // distance = (tmin+t) * -0.2
    payload.radiance = vec3(0.0);
    payload.distance = (gl_RayTminEXT + gl_HitTEXT) * -0.2;
} else {
    payload.radiance = albedo * directLighting(pos, normal)
        + albedo * sampleIrradiance(pos, normal, cameraPos.xyz)
        * infiniteBouncesMultiplier;
    payload.distance = gl_RayTminEXT + gl_HitTEXT;
}

// miss
payload.radiance = vec3(0.529, 0.807, 0.921);
payload.distance = 1000.0;
```

`shadeHitPoint` hides index, vertex, UV, and normal fetches. No normal map is needed: probe resolution erases that detail.

## 30.4 Push probes out of walls

Grid placement drops some probes inside walls, where they contribute blackness and smear neighbours. Offsetting runs once at volume creation. It first counts backface rays per probe and early-outs when the probe is clearly in open space:

```glsl
int backfaces = 0;
for (int r = 0; r < PROBE_RAYS; ++r) {
    vec4 s = texelFetch(radianceTex, ivec2(r, probeIndex), 0);
    if (s.w < 0.0)
        ++backfaces;
}
if (backfaces < int(0.1 * float(PROBE_RAYS)))
    return; // fewer than 10% of rays hit backfaces: nothing to fix
```

If over a quarter of a probe rays report negative (backface) distances, the probe is inside geometry, so slide it away from the closest backface direction, clamped to max_probe_offset * probe_spacing so cells keep their identity (half the probe spacing only when max_probe_offset is 0.5). If the closest frontface is under about 5 cm, nudge along the farthest frontface direction instead. Run about five iterations, then freeze - motion makes light flicker. Later stages add the stored `vec3` offset to the grid position before use.

## 30.5 Blend rays into filtered atlases

One compute dispatch updates irradiance, a second updates visibility with the same shape. Atlas layout is a packed grid of per-probe tiles: for example, a 3x2x4 probe grid with 8x8 texels per probe plus a 1-texel border on every side stores each probe in a 10x10 tile, so the full atlas is 30x20x40 texels unfolded into 2D. Each thread owns one atlas texel; border threads take a copy path. Interior threads decode their octahedral direction, accumulate their probe rays by alignment weight, normalise, and blend against last frame:

```glsl
ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
vec3 texelDir = octDecode(normalizedOctCoord(texel));
vec4 accum = vec4(0.0);
for (int r = 0; r < PROBE_RAYS; ++r) {
    vec3 rayDir = normalize(randomRotation * sphericalFibonacci(r, PROBE_RAYS));
    float w = max(0.0, dot(texelDir, rayDir));
    if (w < EPSILON)
        continue;
    vec4 s = texelFetch(radianceTex, ivec2(r, probeIndex), 0);
    if (s.w < 0.0)
        continue;
    accum += vec4(s.rgb * ENERGY_CONSERVATION * w, w);
}
if (accum.w > EPSILON) {
    accum.xyz /= accum.w;
    accum.w = 1.0;
}
// imageLoad takes an ivec2 texel coordinate, not a float oct coord.
vec4 prev = imageLoad(irradianceImg, texel);
accum = mix(accum, prev, hysteresis);
imageStore(irradianceImg, texel, accum);
```

Visibility repeats the loop storing clamped distance and squared distance, which the sampler needs for its variance test. Each hit distance is clamped to a maximum of 1.5 times the probe spacing (longer returns are treated as open space) before accumulating mean and mean-squared values. High hysteresis is stable but lags moving lights; drop it after large lighting changes. After a `groupMemoryBarrier()` plus `barrier()`, border threads copy their interior neighbour into the gutter so bilinear filtering stays inside the probe.

## 30.6 Sample eight neighbours per pixel

Sampling runs at quarter resolution, picking the maximum depth value per 2x2 footprint (farthest surface under the standard 0-near/1-far convention) to avoid edge smear. It reconstructs position and normal, biases the lookup slightly along normal plus view to escape self-intersection, finds the enclosing cell, and loops over its eight probes. The self-intersection bias is:

```glsl
vec3 biasedPos = worldPos
    + (normal * 0.2 + V * 0.8) * (0.75 * min_dist) * self_shadow_bias;
```

where `V` is the view vector, `min_dist` is the distance to the closest probe in the cell, and `self_shadow_bias` is a tunable scalar. Each probe contributes its irradiance texel for the surface normal, scaled by trilinear weight times a Chebyshev visibility weight with signature:

```glsl
float chebyshevWeight(vec3 probePos, vec3 fragPos, vec3 fragNormal,
                      vec2 meanDistSq, float bias);
```

When the point sits beyond the mean occluder distance, confidence falls as `variance / (variance + diff * diff)`, cubed for contrast and floored at 0.05. The per-probe trilinear factor must be recomputed inside the loop from the per-axis blend `alpha`: a single invariant weight shared by all eight probes destroys the spatial blend.

```glsl
const float PI = 3.14159265359;
vec3 alpha = clamp((biasedPos - basePos) / probeSpacing, vec3(0.0), vec3(1.0));
vec3 sumIrr = vec3(0.0);
float sumW = 0.0;
for (int i = 0; i < 8; ++i) {
    ivec3 off = ivec3(i, i >> 1, i >> 2) & ivec3(1);
    vec3 trilinear = mix(vec3(1.0)-alpha, alpha, vec3(off));
    float trilinearW = trilinear.x * trilinear.y * trilinear.z;
    ivec3 pc = clamp(base + off, ivec3(0), probeCounts - ivec3(1));
    float w = chebyshevWeight(probePosOf(pc), biasedPos, normal,
        textureLod(visibilityTex, probeUV(biasedPos, pc), 0).rg, bias);
    w *= trilinearW + 0.001;
    sumIrr += w * textureLod(irradianceTex, probeUV(normal, pc), 0).rgb;
    sumW += w;
}
return 0.5 * PI * sumIrr / max(sumW, 1e-4);
```

The `0.5 * PI` factor converts the cosine-weighted average back to irradiance. The result lands in a small indirect-lighting texture.

## 30.7 Add the bounce to lighting

The deferred lighting shader fetches that texture after direct terms, multiplies by albedo, masks by the diffuse remainder, and adds:

```glsl
vec3 V = normalize(cameraPos - worldPos);
vec3 F = fresnelSchlickRoughness(max(dot(normal, V), 0.0), F0, roughness);
vec3 kD = (1.0 - F) * (1.0 - metallic);

vec3 indirectIrr = textureLod(indirectTex, screenUV, 0).rgb;
finalColour.rgb += kD * indirectIrr * baseColour.rgb * ao;
```

Natural extensions: skip dead probes, add camera-centred cascades, and adapt hysteresis to luminance change.

## Recap

- Indirect light is bounced light; the G-buffer holds only the first hit, so probes fill the rest.
- DDGI caches the bounce in a regular grid with irradiance (colour) and visibility (distance mean and variance) atlases, octahedrally mapped with filter borders.
- Raygen fires rotated Fibonacci rays per probe row; hit shades remote direct light; miss returns sky; backfaces flag negative distance.
- Offsets push buried probes out a few times at setup, clamped to max_probe_offset * probe_spacing, then freeze.
- Update shaders accumulate by direction, blend with hysteresis, and fix borders.
- Sampling blends eight neighbours by trilinear times Chebyshev weights at quarter resolution.
- Lighting adds `kD * albedo * indirectIrradiance` after direct terms.

## Exercises

1. **Backface probe hunt.** Place a volume so probes land inside a wall. Dump one buried probe row of distances, count negatives, and confirm the inside test (backfaces / rays > 0.25) fires. Step the offset shader five times and record movement versus half spacing.
2. **Hysteresis versus motion.** Animate a point light across a fixed camera at hysteresis 0.9, 0.5, and 0.0. Compare flicker against lag in the indirect texture, then state a rule for dropping hysteresis automatically on large luminance change.
