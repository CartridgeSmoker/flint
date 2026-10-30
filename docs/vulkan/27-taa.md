# Chapter 27: Temporal Anti-Aliasing

> **Objectives:** Explain why deferred rendering pushed games from MSAA to TAA; jitter the projection each frame; reproject history with per-pixel motion vectors; clamp history with a neighborhood AABB; reject disoccluded samples without ghosting. Chapter 26 supplies the jittered dither and temporal reprojection this chapter accumulates, and Chapter 28 reuses jittered sampling for ray-traced reference images.
>
> **Flint note:** Apply TAA to the 3D viewport only. Flint's UI text, dimension lines, and panel glyphs must stay crisp, so render overlays after TAA or in a separate pass with TAA disabled. If viewport edges shimmer during orbit but labels stay sharp, the split is working.

## 27.1 From MSAA to sampling across time

Super-sampling is simple in principle: shade more samples than pixels, then filter down. Multi-Sample Anti-Aliasing (MSAA) did that per frame, and it suited forward renderers with simple shading. Deferred rendering broke the bargain: shading from G-buffer textures makes per-sample costs explode in time and bandwidth. Several engines tried to marry MSAA to deferred; few shipped it.

The detour was post-process anti-aliasing. Morphological Anti-Aliasing (MLAA, Reshetov 2009, first on CPU with SSE, later on Cell SPUs, then on GPU by Jimenez in 2011) found edges in the finished image and blended them. SMAA added pattern lookup and an early temporal component. These smoothed silhouettes but could not recover sub-pixel detail: the samples were never shaded.

TAA takes the other path. Shade one jittered sample per frame and accumulate across frames. A static camera converges toward a super-sampled image; a moving camera reprojects history along motion vectors. The cost is roughly one fullscreen pass plus a velocity buffer, which is why TAA became the default wherever deferred rendering rules.

## 27.2 Jittered sampling: a different sub-pixel each frame

Each frame, shift the projection matrix by a fraction of a pixel so the rasterizer samples a slightly different point inside every pixel. Over a cycle of 4 or 8 frames the samples cover the pixel area, and accumulation reconstructs the detail.

The shift touches only two elements of the projection matrix. Rebuild the clean projection first, then add the offset; forgetting the reset accumulates drift and produces a swimming image:

```glsl
// x, y are already divided by render width and height.
projection[2][0] += x;
projection[2][1] += y;
```

The offsets come from a low-discrepancy sequence - Halton(2,3), Hammersley, Martin Roberts R2, or interleaved gradient noise - mapped from `[0, 1]` to `[-1, 1]` and divided by resolution. Advance one index per frame and wrap after the period:

```cpp
float jx = halton(jitterIndex, 2) * 2.0f - 1.0f;
float jy = halton(jitterIndex, 3) * 2.0f - 1.0f;
camera.applyJitter(jx / renderWidth, jy / renderHeight);
jitterIndex = (jitterIndex + 1) % jitterPeriod;
```

Store the current and previous jitter in uniforms. Motion vectors need both, because part of every pixel shift comes from the jitter itself rather than scene motion.

## 27.3 Reprojection: motion vectors put history where it belongs

History is last frame's TAA output. To reuse it, each pixel asks where the surface it shows now was on the previous screen. The answer is a per-pixel velocity in an `R16G16_SFLOAT` (`VK_FORMAT_R16G16_SFLOAT`) target cleared to zero each frame. Transparent objects do not write depth, so they must write explicit velocity output or be excluded from history, otherwise they smear.

Static geometry needs only camera motion. Reconstruct world position from depth with the inverse view-projection, transform by the previous view-projection, and subtract the jitter difference so the vector measures true scene motion in NDC. This assumes a `worldFromDepth(uv, depth, invViewProj)` helper that reconstructs world-space position from UV and depth, and that `currNDC` is the current-frame NDC position of that same world point:

```glsl
ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
vec3 world = worldFromDepth(screen_uv, rawDepth, invViewProj);
vec4 prevNDC = prevViewProj * vec4(world, 1.0);
prevNDC.xyz /= prevNDC.w;
vec2 jitterDiff = (jitterNow - jitterPrev) * 0.5;
vec2 velocity = currNDC.xy - prevNDC.xy - jitterDiff; // velocity stored as NDC diff
imageStore(velocityTarget, pos, vec4(velocity, 0.0, 0.0));
```

Dynamic objects need the same math with their previous model matrix, written from the vertex or mesh shader as a second interpolant into the velocity target. Then the naive TAA pass is three fetches and a mix:

```glsl
ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
vec2 velocity = texelFetch(velocityTex, pos, 0).rg;
vec2 reprojected_uv = screen_uv - velocity * 0.5; // velocity stored as NDC diff (NDC [-1,1] to UV [0,1])
vec3 current = texture(colorTex, screen_uv).rgb;
vec3 history = texture(historyTex, reprojected_uv).rgb;
return mix(history, current, 0.1);
```

Get the static-camera case pixel-stable first: with no motion the output should be rock solid. If it crawls, suspect matrix order or a missing jitter term. What remains is ghosting wherever reprojection was wrong, which is the rest of the chapter.

> **Misconception: "The center pixel's velocity is the right reprojector."**
> At a silhouette, the center pixel may sit on background while most of its footprint covered foreground last frame, or vice versa. Sampling velocity at the center drags background history across foreground edges and leaves trails. The fix is reading velocity at the closest surface in a small neighborhood, not at the center.

## 27.4 Resolve and clamping: trust history, but verify

A good reprojector still fails where surfaces appear or disappear. The standard defense builds a validity box from the current frame: sample a 3x3 neighborhood around the pixel, record per-channel minimum and maximum, plus the first and second color moments for variance clipping:

```glsl
ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
vec3 nmin = vec3(1e4), nmax = vec3(-1e4), m1 = vec3(0.0), m2 = vec3(0.0);
for (int y = -1; y <= 1; ++y)
for (int x = -1; x <= 1; ++x) {
    vec3 s = texelFetch(colorTex, clamp(pos + ivec2(x, y), ivec2(0), resolution - 1), 0).rgb;
    nmin = min(nmin, s); nmax = max(nmax, s);
    m1 += s; m2 += s * s;
}
```

History outside this box cannot plausibly belong to the current surface, so pull it inside. The most robust variant clamps to the min/max box first, then clips toward the variance box (`mu +/- gamma * sigma`, `gamma` near 1.0):

```glsl
vec3 mu = m1 / 9.0;
vec3 sigma = sqrt(abs(m2 / 9.0 - mu * mu));
vec3 clamped = clamp(history, nmin, nmax);
vec3 minc = mu - sigma;
vec3 maxc = mu + sigma;
history = clip_aabb(minc, maxc, vec4(clamped,1), 1.0f).rgb;
```

The resolve then blends with adaptive weights instead of a fixed 0.1. Flat areas trust history (weight near 0.9); high-contrast neighborhoods, luminance jumps, or single-pixel fireflies favor the current frame. Three resolve filters cover the range:

1. Temporal mix - fixed blend factor between history and current:

```glsl
float currentW = 0.1;
float historyW = 0.9;
vec3 result = (current * currentW + history * historyW)
            / max(currentW + historyW, 1e-5);
// equivalent: result = mix(history, current, currentW / (currentW + historyW));
```

2. Luminance-difference weighting - history weight falls as frame luminance diverges:

```glsl
float lumC = luminance(current);
float lumH = luminance(history);
float lumDiff = abs(lumC - lumH);
float currentW = clamp(lumDiff * 4.0, 0.05, 1.0);
float historyW = 1.0 - currentW;
vec3 result = (current * currentW + history * historyW)
            / max(currentW + historyW, 1e-5);
```

3. Inverse-luminance weighting - bright fireflies get less history to avoid trails:

```glsl
float lumC = luminance(current);
float currentW = 0.05 + 0.25 / (1.0 + lumC * 4.0);
float historyW = 1.0 - currentW;
vec3 result = (current * currentW + history * historyW)
            / max(currentW + historyW, 1e-5);
```

The current-frame scene sample itself should use a constrained filter so one jittered tap does not inject an outlier. Use a `subsample_filter` block:

```glsl
// Constrained scene sample for the current frame.
vec3 scene = subsample_filter(colorTex, screen_uv, nmin, nmax, mu, sigma);
```

`subsample_filter` supports four constraint modes:

- `SUBSAMPLE_UNCONSTRAINED` - raw point sample, no neighborhood check.
- `SUBSAMPLE_CLAMP` - clamp the tap into the `nmin`/`nmax` box (default).
- `SUBSAMPLE_CLIP` - clip the tap against the `minc`/`maxc` AABB segment.
- `SUBSAMPLE_VARIANCE_CLIP` - clip against the `mu +/- gamma * sigma` variance box.

The default is `SUBSAMPLE_CLAMP`; use the variance modes only where fireflies persist.

A filtered history fetch (Catmull-Rom on the reprojected UV) sharpens accumulation. Recover texture detail afterwards with one of three sharpening methods, kept modest because sharpening accentuates the aliasing TAA just removed:

1. Negative mip bias - sample the current frame with bias `-0.25`:

```glsl
vec3 sharp = textureLod(colorTex, screen_uv, -0.25).rgb;
```

2. Unjittered resolve - remove the frame jitter before the final tap with an unjitter UV function, then shade/sample settled detail:

```glsl
vec2 unjittered_uv = unjitter_uv(screen_uv, jitterNow);
vec3 settled = texture(colorTex, unjittered_uv).rgb;
```

3. Luminance sharpen - high-pass on luminance added back with a small gain (0.1-0.2):

```glsl
float lumAvg = dot(scene, vec3(0.333));
vec3 sharp = scene + (scene - vec3(lumAvg)) * 0.15;
```

## 27.5 Disocclusion: the pixel with no past

Disocclusion is the hard case: a background pixel newly revealed from behind motion has no valid history. Its reprojected UV points at whatever the foreground used to cover. Clamping catches most of it, but thin features and fast motion leak through as brief smears.

Three defenses stack. First, reproject with the closest depth in the 3x3 neighborhood (Brian Karis's trick): the nearest surface dominates the footprint, so its velocity is the best guess:

```glsl
ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
float bestDepth = 1.0; ivec2 bestPos = pos;
for (int y = -1; y <= 1; ++y)
for (int x = -1; x <= 1; ++x) {
    ivec2 p = clamp(pos + ivec2(x, y), ivec2(0), resolution - 1);
    float d = texelFetch(depthTex, p, 0).r;
    if (d < bestDepth) { bestDepth = d; bestPos = p; }
}
vec2 velocity = texelFetch(velocityTex, bestPos, 0).rg;
vec2 reprojected_uv = screen_uv - velocity * 0.5; // velocity stored as NDC diff (NDC [-1,1] to UV [0,1])
```

Second, reject on disagreement: if reprojected depth differs sharply from current depth, or neighboring velocities diverge, drop history weight toward zero for that pixel. Third, let the resolve filters dilute the one wrong frame quickly rather than persisting it across the jitter cycle.

Animated dithering rides the same machinery. Volumetric fog and per-pixel lighting dither from Chapter 26 look noisy alone, but TAA treats successive offsets as extra samples and converges them. Keep dither small enough that one frame of it survives clamping; oversized noise reads as disocclusion every frame and never settles. As a fog-dither-scale caveat: scale the fog dither amplitude with resolution and jitter magnitude (smaller at lower resolutions and larger jitter steps), otherwise the dither exceeds the neighborhood box and TAA rejects it as motion every frame.

## Recap

- MSAA shades extra samples per frame; deferred rendering made that unaffordable, and edge-only post-process methods could not recover detail, so TAA accumulates jittered samples across frames.
- Jitter shifts the projection by sub-pixel offsets from a Halton or R2 sequence; always reset the projection before adding, and ship both jitters to shaders.
- Per-pixel motion vectors reproject history; static geometry derives them from depth, dynamic objects write them from shaders, and jitter must be subtracted out.
- A 3x3 neighborhood builds a color AABB plus variance moments; clamping and clipping history into it removes most ghosting, and adaptive resolve weights finish the blend.
- Disocclusion has no valid history: use closest-depth velocity, depth-difference rejection, and fast history dilution, and keep dither small enough to converge.

## Exercises

1. **Static-camera convergence.** Disable camera and object motion with a 4-frame Halton cycle, and log one edge pixel over 8 frames. Confirm it settles instead of cycling, then swap in R2 and interleaved gradient noise and compare edge sharpness.
2. **Disocclusion stress.** Slide a thin foreground bar across a textured background at several speeds. Toggle closest-depth reprojection versus center sampling, then sweep the variance `gamma` from 0.75 to 1.5 and record where trails vanish versus where detail starts to flicker.
