# Chapter 26: Volumetric Fog

> **Objectives:** Explain scattering with phase, extinction, and transmittance; build a froxel volume; inject, light, and integrate fog; tame banding with spatial and temporal filtering plus jitter. Chapter 27 reuses the temporal reprojection and jitter introduced here for antialiasing, and Chapter 23 covers the cluster bins and shadow maps that Section 26.4 lights with.

> **Flint note:** Flint viewports span meters to kilometers, so distance cueing matters more than thick smoke. Run this volume at quarter resolution or lower and drive density from view distance; injection and apply below map onto Flint's inspection fade.

## 26.1 Fog is light scattered inside a volume

Surface fog blends by distance. Volumetric fog tracks what light did crossing the air: some photons were absorbed, some scattered away, some scattered into the eye from nearby lights. Those three terms - absorption, out-scattering, in-scattering - are the whole effect.

Three helpers make them computable. The phase function says how strongly a froxel throws light toward the viewer given the angle between view and light directions. Extinction says how quickly light dies per meter. Transmittance integrates extinction with the Beer-Lambert law, `T = exp(-extinction * distance)`.

This guide uses the Henyey-Greenstein phase function, the standard cheap approximation with one anisotropy parameter `g` in `(-1, 1)`. Forward-scattering fog uses positive `g` around 0.3 to 0.6; `g = 0` is isotropic:

```glsl
float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    float denom = pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
    return (1.0 - g2) / (4.0 * 3.14159265 * denom);
}
```

`cosTheta` is `dot(viewDir, -lightDir)`.

## 26.2 A frustum-aligned volume makes it real-time

Fog is low-frequency: it changes slowly across the screen. Following Wronski's Siggraph 2014 approach, evaluate it in a small 3D texture aligned to the camera frustum, often 128 x 128 x 128 or quarter screen resolution, then upsample at apply time. Each cell is a froxel, a frustum voxel.

Depth slices use an exponential distribution so near slices stay thin and far slices grow thick. One thread per froxel runs each stage, and the pipeline is five compute dispatches:

```
inject (density + albedo) -> scatter (light * phase) -> filter (spatial + temporal)
  -> integrate (front-to-back raymarch) -> apply (modulate scene color)
```

Injection writes scattering and extinction per froxel. Scattering evaluates lights. Filtering denoises. Integration marches down Z once and stores, per slice, accumulated light plus transmittance from the camera to that slice. The scene shader then needs one 3D fetch per pixel: `color = color * transmittance + scattering`.

## 26.3 Inject density, store scattering and extinction

Injection converts artistic controls - a constant haze, a height falloff, a colored box volume - into the two quantities the rest of the pipeline needs:

```glsl
vec4 scatteringExtinction(vec3 color, float density) {
    float extinction = scatteringFactor * density;
    return vec4(color * extinction, extinction);
}
```

The dispatch resolves each froxel to a world position, perturbs density with tiled 3D noise scrolled for wind, and accumulates. The noise texture holds 4-octave tiled Perlin noise sampled with a repeat (wrap) sampler so `worldPos * noiseScale` tiles seamlessly; scrolling by `windDir * frameIndex * noiseSpeed` animates it:

```glsl
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec3 coord = ivec3(gl_GlobalInvocationID.xyz);
    vec3 worldPos = worldFromFroxel(coord);
    float fogNoise = texture(globalTextures3D[noiseIndex],
        worldPos * noiseScale + windDir * float(frameIndex) * noiseSpeed).x;

    vec4 se = vec4(0.0);
    se += scatteringExtinction(vec3(0.5), densityModifier * fogNoise);
    se += scatteringExtinction(vec3(0.5),
        heightDensity * exp(-heightFalloff * max(worldPos.y, 0.0)) * fogNoise);

    vec3 d = abs(worldPos - boxCenter);
    if (all(lessThanEqual(d, boxHalfExtent)))
        se += scatteringExtinction(unpackColor(boxPackedColor).rgb, boxDensity * fogNoise);
    imageStore(globalImages3D[froxelDataIndex], coord, se);
}
```

No lighting here, only material.

## 26.4 Light each froxel like a tiny fog particle

Scattering reuses the clustered structures from Chapter 23: from the froxel world position, fetch the light range for its bin and tile, then test only those lights. There is no BRDF, only attenuation, shadow, and phase. UVW coordinates are normalized texel centers:

```glsl
// rcpFroxelDims = 1.0 / vec3(volumeDims).
vec3 froxelToUVW(ivec3 c) { return (vec3(c) + 0.5) * rcpFroxelDims; }
```

```glsl
vec4 dataTexel = texture(globalTextures3D[froxelDataIndex], froxelToUVW(froxelCoord));
vec3 lighting = vec3(0.0);
vec3 V = normalize(cameraPos - worldPos);
// Froxel-slice-to-bin mapping: worldPos already encodes the slice depth,
// so reuse the Ch. 23 Sec. 23.6 bin lookup. Bins pack (max << 16) | min,
// 1-based, with zero meaning empty.
vec4 posView = worldToCamera * vec4(worldPos, 1.0);
float linearD = (-posView.z - zNear) / (zLightFar - zNear);
uint binIndex = min(uint(linearD / BIN_WIDTH), uint(NUM_BINS) - 1u);
uint binWord = bins[binIndex];
uint binMin = binWord & 0xFFFFu;
uint binMax = (binWord >> 16u) & 0xFFFFu;
// Froxel XY covers the full screen, so scale to pixels then to tiles.
uvec2 tileCoord = uvec2((vec2(froxelCoord.xy) + 0.5) * rcpFroxelDims.xy * vec2(resolution) / float(TILE_SIZE));
if (binMax != 0u) {
    binMin -= 1u; binMax -= 1u; // stored 1-based; zero means empty
    for (uint id = binMin; id <= binMax; ++id) {
        if (!tileCoversLight(tileCoord, id))
            continue;
        vec3 L = lightPos(id) - worldPos;
        float dist = length(L);
        float shadow = shadowForLight(id, worldPos);
        float atten = squareFalloff(dist, lightRadius(id)) * shadow;
        lighting += lightColor(id) * lightIntensity(id)
            * phaseHG(dot(V, -L / dist), anisotropy) * atten;
    }
}
imageStore(globalImages3D[scatterIndex],
    froxelCoord, vec4(dataTexel.rgb * lighting, dataTexel.a));
```

> **Misconception: "Volumetric lighting needs its own light culling."**
> It does not. The froxel already has a world position, so the existing cluster bins and shadow maps answer which lights reach it. The only new math is multiplying by the phase function instead of a surface BRDF.

## 26.5 Integrate once, apply with one fetch

Integration is a short raymarch baked into the volume so the scene shader avoids marching per pixel. One thread per XY column walks Z front to back, using each slice thickness `zStep` for Beer-Lambert:

```glsl
vec3 accumScatter = vec3(0.0);
float accumTrans = 1.0;
float prevZ = froxelNear;
for (int z = 0; z < depthSlices; ++z) {
    ivec3 c = ivec3(xy, z);
    vec4 s = texture(globalTextures3D[scatterIndex], froxelToUVW(c));
    float nextZ = sliceToLinearDepth(z + 1);
    float zStep = max(nextZ - prevZ, 1e-4);
    prevZ = nextZ;

    float T = exp(-s.a * zStep);
    vec3 segScatter = (s.rgb - s.rgb * T) / max(s.a, 1e-5);
    accumScatter += segScatter * accumTrans;
    accumTrans *= T;
    imageStore(globalImages3D[integratedIndex], c, vec4(accumScatter, accumTrans));
}
```
One fetch applies it at the end of lighting. Upsampling is deferred to this fetch: the volume stays at quarter resolution through filter and integrate, and the single trilinear sample below magnifies to screen resolution:

```glsl
vec3 applyVolumetricFog(vec2 screenUV, float rawDepth, vec3 color) {
    float linearDepth = rawDepthToLinear(rawDepth, fogNear, fogFar);
    float w = linearDepthToSliceUV(fogNear, fogFar, linearDepth, depthSlices);
    vec4 st = texture(globalTextures3D[integratedIndex], vec3(screenUV, w));
    return color * st.a + st.rgb;
}
```

## 26.6 Banding is the real enemy; filter and jitter it away

A 128-deep volume banded badly without help. Two filters plus deliberate noise fix most of it.

Spatial filtering is a small 2D Gaussian over X and Y only, never Z, so depth edges survive. The kernel is RADIUS 2 (5 x 5) with SIGMA ~1.0, reading the injected volume and writing the scratch image; the temporal pass below then reads scratch and writes scatter, so the two stages ping-pong in opposite directions. It softens voxel edges but cannot remove banding alone.

Temporal filtering does the heavy work. Reproject each froxel center through the previous view-projection without the current jitter, and blend when the history UVW lands inside `[0, 1]`:

```glsl
vec4 cur = texture(globalTextures3D[scratchIndex], froxelToUVW(coord));
if (useTemporal == 1) {
    vec4 prevClip = prevViewProj * vec4(worldFromFroxelNoJitter(coord), 1.0);
    vec3 ndc = prevClip.xyz / prevClip.w;
    vec3 huv = vec3(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5,
        linearDepthToSliceUV(fogNear, fogFar, rawToLinear(ndc.z), depthSlices));
    if (all(greaterThanEqual(huv, vec3(0.0))) && all(lessThanEqual(huv, vec3(1.0)))) {
        vec4 history = textureLod(globalTextures3D[historyIndex], huv, 0.0);
        cur = mix(history, cur, temporalBlend); // 0.05-0.15 weight of new frame
    }
}
imageStore(globalImages3D[scatterIndex], coord, cur);
```

Jitter makes the blend converge instead of ghost. Offset the froxel mapping each frame with blue noise advanced by `fract(noise + frame * 0.61803398875)`, scaled to a fraction of a slice. Static views accumulate sub-slice positions; moving cameras fall back to the noisier single frame, hence the validity test and modest blend.

## Recap

- Participating media means absorption plus in/out-scattering; phase, extinction, transmittance compute it.
- A froxel volume with exponential depth evaluates fog far below screen resolution.
- Injection writes scattering and extinction from constant, height, and box densities with animated noise.
- Scattering reuses cluster bins and shadow maps, swapping the BRDF for a phase term.
- One front-to-back march bakes scattering and transmittance; the scene applies fog with one fetch.
- A 2D Gaussian plus reprojected temporal blending with blue-noise jitter removes most banding.

## Exercises

1. **Slice-count sweep.** On a height-fog view with temporal off, render 32, 64, and 128 slices. Re-enable blending at 0.1 and record frames to convergence for each.
2. **Anisotropy check.** Put one shadowed spotlight in a dark box volume, sweep phase `g` through 0.0, 0.3, 0.7, and note shaft brightness looking toward the light. Explain why negative `g` suits backlit dust.
