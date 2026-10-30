# Chapter 10: Surfaces In Depth - Lifecycle, Frames, Damage, Regions, Subsurfaces, and HiDPI

> **Objectives:** Explain pending versus current state and commit atomicity; pace rendering with `wl_surface.frame` callbacks; report damage with `damage_buffer`; constrain blending and input with `wl_region`; build trees with `wl_subsurface`; adapt to output scale with `set_buffer_scale`.

## 10.1 Pending state, current state, and commit

Each `wl_surface` holds two copies of its state. Current state is what the compositor shows. Pending state is scratch space the client edits with commits on this surface - `attach`, `damage`, `damage_buffer`, `set_opaque_region`, `set_input_region`, `set_buffer_scale`, `set_buffer_transform` (rotate the buffer or, via viewports, present a subset) - versus subsurface position and z-order commits on the parent: `set_position`/`place_above`/`place_below` are buffered state of the parent and apply on the parent's commit, unaffected by sync/desync mode. Nothing becomes visible until `wl_surface.commit` promotes pending to current in one atomic step.

Atomicity means the compositor never observes a half-applied frame: new buffer with old damage, or new scale with old regions. Either the whole pending batch lands or none of it does.

Resending overwrites. Pending is not a queue: attaching buffer A then buffer B before committing displays only B, and setting position `(10,10)` then `(20,20)` moves once to the second value. The first commit maps: a fresh surface is sizeless and invisible until a commit carrying a non-null buffer (plus an assigned role such as `xdg_surface` and any role-specific state, e.g. an xdg configure ack) asks the compositor to show it - buffer plus role alone are insufficient without the role's own readiness. Committing with a null buffer hides it again.

This motivates the `redraw()` habit. Centralise every pending edit in one function - pick a buffer, paint it, attach it, describe damage, set regions and scale - then commit once. Input handlers record intent ("caret moved", "scale changed") and request a redraw; they never scatter `attach` calls.

```text
          attach / damage / set_*         commit
 [ edits ] ───────────────────────► [ pending ] ──────► [ current ] ──► screen
    ▲                                   │                     │
    └──── resend overwrites ────────────┘   no commit = no effect
```

Only the `commit` arrow is observable. When debugging, ask what sits in pending and when it last committed.

## 10.2 Frame callbacks: permission to draw

A new client renders blind. It does not know whether the output runs at 60 Hz or 144 Hz, whether its window is hidden, or whether the compositor throttles background surfaces. Drawing on a fixed timer stutters on faster displays and burns CPU painting hidden windows nobody sees.

`wl_surface.frame` inverts control. The client requests a `wl_callback`; the compositor sends its `done` event with a millisecond timestamp when fresh pixels are actually wanted, roughly as the previous frame goes out. That `callback_data` timestamp counts milliseconds from an UNSPECIFIED epoch - useful only for deltas, never for wall-clock time; for nanosecond-precise presentation feedback see the `presentation-time` extension. No `done`, no draw. Hidden windows stop costing anything and the client adapts to any refresh rate without configuration.

Keep at most one callback outstanding. The cycle is: request one frame, wait for `done`, destroy the callback inside the handler, paint and commit, then immediately request the next frame for continuous animation. The compositor only answers requests, so the client must kick off the first one manually after its initial commit. Static windows skip this entirely: commit once and never request a frame.

Drive animation from the timestamp, not from a per-frame step. A fixed step runs faster at 144 Hz than at 60 Hz; elapsed-time integration runs identically everywhere. At 24 pixels per second, `x += 24 * delta_seconds` moves 24 pixels in one second on any display. If a throttled window receives `done` three seconds late, the next frame jumps 72 pixels and stays on schedule instead of falling behind.

```c
static struct wl_surface *surface;
static struct wl_callback *frame_cb;
static uint32_t last_time = 0;
static float offset = 0.0f;

static void redraw(uint32_t *pixels, int w, int h, float scroll);

static void
frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	wl_callback_destroy(cb); /* one-shot: always destroy here */
	frame_cb = NULL;

	if (last_time != 0)
		offset += (time - last_time) / 1000.0f * 24.0f;
	last_time = time;

	/* 16 ms delta yields ~0.38 px/frame (24 px/s); float
	 * accumulator preserves sub-pixel progress that an
	 * integer step would truncate to zero at 60 Hz. */
	redraw(NULL, 640, 480, offset); /* pseudocode: paint into shm
	 * buffer here, then attach + damage_buffer; see Ch.8
	 * draw_frame for a real SHM signature. */
	wl_surface_commit(surface);

	frame_cb = wl_surface_frame(surface); /* re-arm immediately */
	static const struct wl_callback_listener listener = {
		.done = frame_done,
	};
	wl_callback_add_listener(frame_cb, &listener, NULL);
}
```

Destroy in the handler (the object never fires twice), re-arm there too (otherwise animation stalls after one frame), and seed `last_time` so the first delta is zero rather than a jump from timestamp zero.

> **Misconception: "Commit one request at a time"?**
> No. Beginners write `attach`, `commit`, `damage`, `commit`, `set_opaque_region`, `commit`, as if each request needs its own commit. That publishes three intermediate states that can flash on screen. Batch every `attach`, damage, region, and scale change into pending state, then issue exactly one `commit`. Requests are words; `commit` is the full stop.

## 10.3 Damage: what changed

Attach selects the pixel store; damage selects which rectangles within it differ from the previous frame. The split lets the compositor skip uploads and repaint for unchanged areas, which dominates performance for large windows with small updates such as a caret or a clock.

Prefer `wl_surface.damage_buffer` over `wl_surface.damage`. Both take `x, y, width, height`, but `damage` uses surface coordinates while `damage_buffer` uses buffer coordinates. They agree at scale 1 and diverge at scale 2, where one surface unit covers 2x2 buffer pixels. Buffer coordinates always match the bytes just painted, so new code should use `damage_buffer` throughout.

Partial damage wins for small updates: a 2x20 caret blink or a once-per-second clock digit should not force a full-window upload. When tracking dirty rectangles is impractical - fresh attach, resize, unknown provenance - damage everything with the `INT32_MAX` idiom:

```c
wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
```

Damage is double-buffered: it accumulates in pending state and applies at commit, never leaking from one buffer onto the next unless re-issued. The classic bug is attach without damage - new pixels committed, nothing marked changed, compositor keeps showing cached tiles, window looks blank or stale. The fix is ritual: every `attach` is followed by a matching `damage_buffer` before `commit`.

## 10.4 Regions: opaque hints and input shapes

A `wl_region` is a rectangle set built with `wl_compositor.create_region`, `region.add`, and `region.subtract`, then installed with `set_opaque_region` or `set_input_region`. The object is only a carrier: adding rectangles does nothing until set plus commit, and after commit the compositor holds its own copy, so destroy the region promptly. Updating a region after `set_*` does not update the surface: the surface copies the rectangles at `set_*` time, so later `add`/`subtract` calls on the same region object have no effect until the next `set_*` plus commit.

Opaque regions skip blending. By default the opaque region is EMPTY (everything blends) while the default input region is the WHOLE surface (every pixel takes input), so the standard example declares a 512x512 window opaque except for a 256x256 translucent hole:

```c
struct wl_region *region = wl_compositor_create_region(compositor);
wl_region_add(region, 0, 0, 512, 512);
wl_region_subtract(region, 128, 128, 256, 256);
wl_surface_set_opaque_region(surface, region);
wl_surface_commit(surface);
wl_region_destroy(region);
```

Errors here cost either correctness (claiming opaque over translucent pixels misrenders) or speed (omitting truly opaque pixels blends needlessly). When unsure, leave pixels out.

Input regions drive hit-testing: clicks land according to the region, not the visible pixels. A window with a blurred shadow excludes the shadow margin so clicks fall through to whatever is beneath. A circular palette sets an approximating region so clicks on transparent corners pass through. Passing `NULL` clears: null opaque means nothing is opaque, null input means the whole surface takes input. Both setters are commit-gated, and destroying the region object after `set_*` but before `commit` is safe because the surface copies the rectangles at `set_*` time.

## 10.5 Subsurfaces: core's only role

Core Wayland defines exactly one surface role: `wl_subsurface`. Top-level windows, popups, layers, and cursors all come from extensions. `wl_subcompositor.get_subsurface` binds a child surface to a parent so one window can be tiled from several surfaces:

```c
struct wl_subsurface *sub = wl_subcompositor_get_subsurface(subcompositor,
	child_surface, parent_surface);
wl_subsurface_set_position(sub, 16, 16);
wl_subsurface_place_above(sub, parent_surface);
wl_subsurface_set_sync(sub);
```

`set_position` offsets the child origin from the parent origin; the child position need not stay within the parent bounds, so overflow and negative offsets are legal. `place_above` and `place_below` order siblings into a local stacking tree, for example a control bar above video or a tooltip above all, and either may reference the parent surface itself to order the child directly above or below its parent. Subsurfaces can nest, forming an ordered tree of arbitrary depth. `set_sync`/`set_desync` apply IMMEDIATELY (they are non-buffered mode switches), unlike the buffered position and stacking state.

Sync mode (the default) batches the child with the parent: committing the child only stages its buffer and damage, which appear when the parent next commits, so parent and child move atomically with no tear. Desync mode (`set_desync`) applies child commits immediately. Position and stacking always behave as if synced regardless of mode, so a child never jumps ahead of its anchor. Default to sync; reserve desync for genuinely independent clocks such as 60 fps video inside a mostly static window. Typical use-cases mix pixel sources that one buffer cannot hold: native-format video plus an RGBA UI overlay, GL-rendered content with software-drawn decorations, or hardware planes and embedded surfaces that must update without redrawing the parent. One surface takes one role forever: a subsurface can never later become a top-level window.

## 10.6 HiDPI: render dense, label it

Layout is logical; pixels are physical. A 256-unit button should look the same size on 96 DPI and 192 DPI panels but uses twice the device pixels on the denser one. Since `wl_output` version 2, each output advertises an integer `scale`, so bind `wl_output` at version >= 2 to receive scale events, and surface `enter`/`leave` events report which outputs a window overlaps. Unaware clients that keep posting scale-1 buffers on scale-2 outputs get upscaled by the compositor (blurry but correctly sized) rather than shown at the wrong size.

Render at the maximum scale among entered outputs. A window spanning scale-1 and scale-2 outputs renders at 2: the compositor downscales cleanly for the scale-1 side, whereas rendering at 1 and upscaling for scale-2 looks blurry. Downscaling degrades gracefully; upscaling does not.

That splits logical from buffer size: a 640x480 window at scale 2 needs a 1280x960 buffer, painted at device resolution, then labelled with `wl_surface.set_buffer_scale` (needs `wl_compositor` version 3) so the compositor maps 2x2 buffer pixels onto each surface unit. The label is pending state and takes effect at commit:

```c
static int current_scale = 1;

/* Pseudocode: redraw() here is shorthand for "paint into an SHM
 * buffer here" (see Ch.8 draw_frame for a real SHM allocate /
 * paint / attach sequence); NULL is not a real pixel pointer. */
static void
rescale(struct wl_surface *surface, int best)
{
	if (best == current_scale)
		return;
	current_scale = best;
	/* Reallocate buffers at 640*scale x 480*scale and repaint. */
	wl_surface_set_buffer_scale(surface, current_scale);
	redraw(NULL, 640 * current_scale, 480 * current_scale, 0);
	wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface);
}
/* Call rescale() with the max scale over entered outputs whenever
 * output scale, enter, or leave events arrive. */
```

Omitting the label is the signature HiDPI bug: a carefully rendered 1280x960 buffer with no scale tag is treated as a 1280x960 logical surface and the window appears at double size. Fractional factors (125%, 150%) layer atop this integer mechanism via extensions such as `wp_fractional_scale_v1`.

## Recap

- Pending versus current: all edits stage invisibly; one `commit` publishes atomically. Resends overwrite, first commit maps, and a central `redraw()` keeps batches coherent.
- `frame`/`done(time_ms)` paces drawing to the display and skips hidden work. Integrate timestamps (24 px/s runs identically at 60 and 144 Hz, jumping correctly after throttle). One callback outstanding, destroy in-handler, re-arm immediately, kick off manually.
- Attach picks pixels; `damage_buffer` in buffer coordinates picks what changed. Damage partially for carets and clocks, fully with `INT32_MAX` after resize, and never attach without damage.
- Regions: `add`/`subtract` a 512-minus-256-hole style set, `set_opaque_region` to skip blending (default empty), `set_input_region` for shadow fall-through and shaped windows. `NULL` clears, commit applies, destroy after set.
- Subsurfaces: `get_subsurface` plus `set_position`/`place_above`/`place_below` builds the tree. Sync (default) batches buffers with the parent; position and stacking always follow the parent.
- HiDPI: track output scale via enter/leave, render at the max, tag with `set_buffer_scale` at commit. Downscale beats upscale; an untagged 2x buffer shows double-size.

## Exercises

1. **Caret damage.** Add a 2x20 caret blinking twice per second to the SHM checkerboard client. First damage the whole surface per blink, then only the caret rectangle. Under `WAYLAND_DEBUG=1`, compare the `damage_buffer` arguments and count damaged pixels per blink. Explain why the partial version scales better with window size.
2. **2x render.** Hard-code scale 2: allocate a 1280x960 buffer for a 640x480 logical window, paint a one-buffer-pixel checkerboard, set scale 2, damage fully, commit. Confirm a crisp 640x480 window. Then remove only `set_buffer_scale`, recommit, and record the displayed size. Explain it in terms of logical versus buffer pixels.
