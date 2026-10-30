# Chapter 31: Reflections

> **Objectives:** Explain screen-space marching and its off-screen limits; fire one roughness-gated ray per pixel with mirror versus GGX VNDF directions; pick a light by importance and shade the hit; denoise with SVGF accumulation, variance, and five wavelet passes; composite the specular term. As the closing chapter, it points back to Chapter 28 for acceleration-structure basics and Chapter 27 for the motion-vector history it reuses.
>
> **Flint note:** Flint viewports are full of low-roughness metal where crisp reflections sell the material. Price it honestly: cutoff near 0.3, half-resolution target, one ray per pixel, SVGF. Full-resolution mirrors stall a CAD session; start gated and half-res.

## 31.1 Marching the depth buffer

Before hardware ray tracing, reflections marched the G-buffer. For each low-roughness pixel, reflect the view vector about the normal, then step along that ray in screen space for a fixed iteration count.

At each step, project the point to a pixel and compare depths. If `abs(rayDepth - sceneDepth)` is under a tuned delta, take that colour. Otherwise keep marching. Miss everything and fall back to an environment sample.

Fixed iterations mean fixed cost in any scene. Step size sets quality: small steps follow curves but cost iterations; large steps skip thin geometry.

## 31.2 Why screen space is not enough

The depth buffer only knows what the camera saw. Anything off screen, behind the camera, or occluded has no depth or colour to fetch, so reflections pop as the view moves: a chrome pipe mirrors a cabinet until the cabinet leaves the frame, then falls back to the environment map.

Coarse marching plus pixel-depth granularity leaves holes, hidden with blur that removes crispness too. Roughness gating saves work but fixes neither limit.

> **Misconception: "More march steps fix disappearing reflections."**
> No. Steps improve hits inside the frame; they cannot recover data that was never rendered. Off-screen geometry, backfaces, and occluded layers are absent from the G-buffer at any step count. Only scene traversal - ray tracing against the acceleration structure - sees them.

## 31.3 One ray per smooth pixel

The ray-traced pass keeps the roughness gate and replaces the march with a real trace. Pixels with `roughness > 0.3` exit; the rest fire exactly one reflection ray. One sample per pixel is noisy by construction; temporal accumulation plus a spatial filter rebuilds it far more cheaply than many samples.

Seed RNG per pixel per frame so noise decorrelates instead of freezing into banding:[^rng]

```glsl
uint rng_state;
uint seed(uvec2 p) { return 19u * p.x + 47u * p.y + 101u; }
uint rand_pcg() {
    uint state = rng_state;
    rng_state = rng_state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state)
        * 277803737u;
    return (word >> 22u) ^ word;
}

// per fragment:
rng_state = seed(gl_LaunchIDEXT.xy) + currentFrame * 1664525u;
float u1 = float(rand_pcg()) * (1.0 / 4294967296.0);
float u2 = float(rand_pcg()) * (1.0 / 4294967296.0);
```

[^rng]: The `+ currentFrame * 1664525u` frame decorrelation term is a Flint adaptation; it deviates from the MGP source RNG seeding, which advances the PCG state per frame without this multiplicative constant.

The reflection pass traces at half resolution and composites the upsampled specular term during the deferred lighting pass; full-resolution tracing is supported for reference captures but stalls interactive sessions, so start gated and half-res.

Direction has two modes. Mirror reflects about the geometric normal for sharp images near zero roughness. Sampled mode draws a microfacet normal from the GGX visible-normal distribution for the pixel roughness, then reflects about that, spreading rays on rougher surfaces. The source VNDF sampler takes tangent-space view plus two roughness (alpha) parameters - `sampleGGXVNDF(V_local, alpha_x, alpha_y, U1, U2)` - so the wrapper below builds a normal basis, transforms, samples isotropically (`roughness, roughness`), and transforms back:

```glsl
vec3 V = normalize(cameraPos - worldPos);
// Wrapper around source sampleGGXVNDF(V_local, roughness, roughness, U1, U2):
vec3 up = abs(normal.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
vec3 T = normalize(cross(up, normal));
vec3 B = cross(normal, T);
vec3 V_local = vec3(dot(V, T), dot(V, B), dot(V, normal));
vec3 H_local = sampleGGXVNDF(V_local, roughness, roughness, u1, u2);
vec3 H = normalize(T * H_local.x + B * H_local.y + normal * H_local.z);
vec3 mirrorDir = normalize(reflect(-V, normal));
vec3 glossyDir = normalize(reflect(-V, H));
```

Trace from an offset origin to escape self-intersection:[^origin]

```glsl
traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0,
    worldPos + normal * 0.01, 0.05, glossyDir, 100.0, 0);
```

[^origin]: The `worldPos + normal * 0.01` origin offset with `tMin 0.05` is a Flint adaptation; it deviates from the MGP source offset distances. Tune per scene scale - larger scenes need larger offsets to escape self-intersection without peter-panning.

Use mirror for calibration and sampled GGX for production.

## 31.4 Shading whatever the ray finds

A hit shades like a tiny deferred pixel. Rather than looping all lights, pick one by importance so bright, close, well-oriented lights win more often. Importance folds spot-cone falloff, surface orientation, and distance into one weight, gated by whether the light is active for the hit point:

```glsl
float theta_i = acos(clamp(dot(-toLightDir, lightDir), -1.0, 1.0));
float theta_u = light.outerConeAngle;
float theta_prime = clamp((theta_u - theta_i) / max(theta_u - light.innerConeAngle, 1e-4), 0.0, 1.0);
float orientation = clamp(dot(hitNormal, toLightDir), 0.0, 1.0) * theta_prime * theta_prime;
bool light_active = (light.range <= 0.0 || distSq < light.range * light.range) && orientation > 0.0;
float importance = light_active ? light.intensity * orientation / max(distSq, 1e-4) : 0.0;
```

Normalise the gated weights to sum to one, draw with the per-pixel RNG, and walk the cumulative distribution:

```glsl
float total = 0.0;
for (uint l = 0; l < activeLights; ++l)
    total += importance[l];
if (total <= 0.0)
    return vec3(0.0); // no active light contributes; skip trace and shading
float r = float(rand_pcg()) * (1.0 / 4294967296.0) * total;
uint lightIndex = 0;
for (lightIndex = 0; lightIndex < activeLights; ++lightIndex) {
    r -= importance[lightIndex];
    if (r <= 0.0)
        break;
}
lightIndex = min(lightIndex, activeLights - 1);
float probability = importance[lightIndex] / total;
```

Then fire a shadow ray toward the chosen light as in Chapter 29 (Ray-Traced Shadows). If visible, evaluate the lighting model at the hit; if occluded, contribute zero. Miss and return sky or environment. Divide by the selection probability so the single sample stays unbiased:

```glsl
vec3 contrib = evalLighting(hitPos, hitNormal, lights[lightIndex]) * visibility;
contrib /= probability;
```

The result is correct on average and speckle in a single frame. That is expected. Do not raise the ray count; run the denoiser.

## 31.5 SVGF temporal accumulation

Spatiotemporal variance-guided filtering rebuilds one-sample data in three stages: accumulate colour and luminance moments over time, estimate variance, then filter spatially with edge-stopping weights. History does most of the work.

Moments are luminance and squared luminance:

```glsl
float l1 = luminance(reflectionColor);
vec2 moments = vec2(l1, l1 * l1);
```

Reproject with the TAA motion vectors, then test before trusting history: inside viewport, same mesh id, depth within ~10x depth derivative, normal within ~16x normal derivative. Any failure means disocclusion, so discard history:

```glsl
if (isConsistent) {
    vec3 histCol = texelFetch(historyColorTex, ivec2(prevCoord), 0).rgb;
    vec2 histMom = texelFetch(historyMomentsTex, ivec2(prevCoord), 0).rg;
    integratedColor = mix(histCol, reflectionColor, 0.2);
    integratedMoments = mix(histMom, moments, 0.2);
} else {
    integratedColor = reflectionColor;
    integratedMoments = moments;
}
```

The 0.2 blend follows motion yet converges static mirrors. Variance follows directly:

```glsl
float variance = max(0.0, integratedMoments.y
    - integratedMoments.x * integratedMoments.x);
```

Note: this guide recomputes variance from the temporally integrated moments each frame and omits explicit variance propagation through the temporal filter (no per-frame variance-boost or history-variance accumulation step). That keeps the implementation short at the cost of slightly underestimated variance after long successful history runs; reintroduce the MGP variance-propagation term if fireflies persist in static mirrors.

## 31.6 Five passes of edge-aware wavelet

The spatial stage runs a 5x5 a-trous cross-bilateral filter five times, doubling stride each iteration so support grows without extra taps.[^atrous] Accumulate kernel weight `h` times edge weight `w(p, q)`:

```glsl
for (int y = -2; y <= 2; ++y)
for (int x = -2; x <= 2; ++x) {
    ivec2 q = p + ivec2(x, y) * (1 << iteration);
    if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, res)))
        continue;
    float h = kernel[x + 2] * kernel[y + 2];
    float w = edgeWeight(p, q);
    vec3 c = texelFetch(integratedTex, q, 0).rgb;
    sumColor += h * w * c;
    sumW += h * w;
}
filteredColor = sumColor / max(sumW, 1e-4);
```

`edgeWeight` multiplies three terms. The normal term `pow(max(0.0, dot(nP, nQ)), sigmaN)` holds on flats and drops across creases. The depth term penalises `abs(zP - zQ)` relative to the depth derivative. The luminance term penalises `abs(lP - lQ)` relative to `sigmaL * sqrt(smoothedVariance)`, with variance pre-smoothed by a 3x3 Gaussian so one noisy tap cannot veto the neighbourhood. Decode octahedral normals first; guard divisions with epsilon.

[^atrous]: A-trous is French for "with holes": tap offsets scale by `1 << iteration` (stride 1, 2, 4, 8, 16 across the five passes), so each 5x5 tap covers a wider footprint without adding texture fetches.

Each iteration feeds the next: early passes remove speckle, later passes spread clean colour while edges hold. The result is the specular term the deferred shader adds, modulated by Fresnel and roughness.

## Recap

- Screen-space reflections march the depth buffer, gate on low roughness, fall back to environment on miss.
- Limits are off-screen absence and coarse-step holes; blur hides holes but kills crispness.
- Ray pass gates near 0.3, seeds RNG per pixel per frame, fires one ray: mirror `reflect` or GGX VNDF sample.
- Hit shades one importance-picked light plus shadow ray, divided by selection probability; output is speckle.
- SVGF accumulates moments with motion reprojection plus viewport, mesh-id, depth, normal tests.
- Variance is `E[x^2] - E[x]^2`; five a-trous wavelet passes apply normal, depth, variance-scaled luminance weights.

## Exercises

1. **Mirror versus GGX spread.** On a fixed view with roughness 0.05, 0.2, and 0.4, capture raw output in mirror and GGX VNDF modes. Measure luminance variance in a flat patch and state where mirror visibly diverges from the sampled lobe.
2. **Break the history, watch the blur.** Disable each consistency test in turn (mesh id, depth, normal) and strafe past an occluder. Record which failure ghosts worst, then raise wavelet iterations from five to seven and describe edges versus residual speckle.
