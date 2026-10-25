# Chapter 8: Surfaces, Buffers, Shared Memory, and Roles

> **Objectives:** Distinguish `wl_surface` from `wl_buffer`; bind `wl_compositor` (v4) and `wl_shm` (v1); allocate anonymous SHM and size `wl_shm_pool` correctly; attach, damage, and commit (including null-buffer hide); read SHM buffers server-side; understand why `dmabuf`/EGL exists; enforce the one-role rule.

> **Flint note:** Flint renders exclusively through Vulkan, so the SHM sections below (8.3-8.6) are background, not the Flint path - Flint presents via the swapchain on a Wayland surface (see `src/vk.c`) instead. What still applies to Flint: surface/role mechanics, the attach-damage-commit ritual, and the dmabuf rationale in 8.8.

## 8.1 Surfaces are endpoints, buffers are stores

`wl_buffer` is an opaque pixel store. Core Wayland never looks inside it. It may live in shared RAM, GPU memory, or elsewhere; only an extension such as `wl_shm` or `linux-dmabuf` knows how it was made.

`wl_surface` is the endpoint the compositor stacks, clips, transforms, and hits-tests. It has a top-left origin `(0,0)`, but is otherwise blank, sizeless, and meaningless until three things combine: a buffer is attached, a role is assigned, and pending state is committed.

Pending versus current state explains the ritual. `attach`, `damage`, `set_opaque_region`, and similar requests only edit pending state. `wl_surface.commit` atomically publishes it. Hence every frame follows attach, damage, commit.

## 8.2 wl_compositor: creating surfaces

Bind `wl_compositor` at version 4 (minimum; see version note below) using the standard string-compare pattern. Fragments in this chapter assume the Chapter 9 includes (`<stdint.h>`, `<string.h>`, `<wayland-client.h>`):

```c
#include <stdint.h>
#include <string.h>
#include <wayland-client.h>

static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL; /* bound in section 8.3 */

static void
registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		uint32_t v = version < 4 ? version : 4;
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, v);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		/* section 8.3: same function, continued */
	}
}
```

`wl_registry_bind` does no clamping: requesting a version above what the compositor advertised is a protocol error. Always take the minimum of advertised and required, as above (here we assume v4 is present for brevity). Then:

```c
struct wl_surface *surface = wl_compositor_create_surface(compositor);
struct wl_region *region = wl_compositor_create_region(compositor);
```

`create_surface` returns the blank endpoint. `create_region` returns a helper built with `wl_region_add`/`wl_region_subtract` and consumed by `set_opaque_region`/`set_input_region`, letting the compositor skip blending and hit-testing. Remember to roundtrip after adding the registry listener or `compositor` is still `NULL`.

Version note: v4 (`wl_compositor`) and v1 (`wl_shm`) above are minimums that every compositor supports. Current versions are `wl_compositor` v6 and `wl_shm` v2. Newer `wl_compositor` adds `damage_buffer` (surface v4, buffer coordinates), `frame` already present, `offset` (v5 for sub-surface positioning), and `preferred_buffer_scale`/`preferred_buffer_transform` (v6); newer `wl_shm` adds `release` (since v2, so clients can destroy the `wl_shm` object). Unless you need those features, binding the minimum keeps the client compatible. `damage_buffer`, buffer scale, and regions are covered in depth in a later chapter; the forward reference here is intentional.

The compositor specification defines the role this way: a compositor "manages zero or more outputs, presents buffers to those outputs, receives input, and maintains a local coordinate system for each surface." A surface is therefore the compositor's per-window coordinate system and input target, not the pixels themselves.

## 8.3 wl_shm: negotiating formats

`wl_shm` is the software path, and it is the ONLY buffer factory defined in `wayland.xml` itself (every other buffer type comes from an extension such as `linux-dmabuf`). Bind version 1 (minimum; current is v2 which adds `release`) and listen for formats. This continues the `registry_global` from section 8.2, merged here rather than redefined:

```c
/* merge into the registry_global from section 8.2: */
} else if (strcmp(interface, wl_shm_interface.name) == 0) {
	uint32_t v = version < 1 ? version : 1;
	shm = wl_registry_bind(registry, name, &wl_shm_interface, v);
}

static void
shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
	/* record format */
}
static const struct wl_shm_listener shm_listener = {
	.format = shm_format,
};
wl_shm_add_listener(shm, &shm_listener, NULL);
wl_display_roundtrip(display);
```

Only `WL_SHM_FORMAT_ARGB8888` and `WL_SHM_FORMAT_XRGB8888` are guaranteed. Anything else is optional. Prefer `XRGB8888` when alpha is unused so the compositor can skip blending.

Little-endian gotcha: on x86, writing `0xFFRRGGBB` as a `uint32_t` is correct, but the bytes in memory are `B G R X`. Filling bytes in `R,G,B` order swaps red and blue. Inverted colors almost always mean a byte-order bug.

## 8.4 Complete SHM helpers

SHM starts as anonymous POSIX shared memory:

```c
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static void
randname(char *buf)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A' + (r & 15) + (r & 16) * 2;
		r >>= 5;
	}
}

static int
create_shm_file(void)
{
	int retries = 100;
	do {
		char name[] = "/wl_shm-XXXXXX";
		randname(name + sizeof(name) - 7);
		--retries;
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);
	return -1;
}

static int
allocate_shm_file(size_t size)
{
	int fd = create_shm_file();
	if (fd < 0)
		return -1;
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}
	return fd;
}
```

`O_CREAT|O_EXCL` guarantees a fresh file, `shm_unlink` right after open makes it anonymous, `0600` keeps it private, the `EEXIST` loop retries name collisions, and the `EINTR` loop retries a signal-interrupted `ftruncate`.

## 8.5 Pool sizing and stride

A `wl_shm_pool` is a byte array; each buffer is a window into it:

```
stride = width * 4       /* XRGB8888, no padding */
frame  = stride * height
pool   = frame * n_buffers
```

For 1920x1080 `XRGB8888`: stride is 7680, one frame is 8294400 bytes (~7.9 MiB), and a double-buffered pool is 16588800 bytes (~15.8 MiB). `wl_shm_pool_create_buffer` takes an `offset`, and `offset + stride * height` must fit inside the pool or the compositor raises a protocol error. Buffer 0 uses offset 0, buffer 1 uses offset 8294400.

Map once, then wrap:

```c
int fd = allocate_shm_file(pool_size);
uint8_t *pool_data = mmap(NULL, pool_size,
	PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, pool_size);
close(fd); /* pool holds its own reference */
```

The pool may be destroyed immediately after its buffers are created. Per `wayland.xml`, "the pool object may be destroyed immediately after creating buffers from it; each buffer keeps its own reference to the pool's storage." Destroying the pool does not free pixels still referenced by live `wl_buffer` objects.

## 8.6 Draw, attach, damage, commit

Create two buffers, draw a checkerboard, and present:

```c
const int width = 640, height = 480;
const int stride = width * 4;
const int frame_size = stride * height;

int fd = allocate_shm_file(frame_size * 2);
uint8_t *data = mmap(NULL, frame_size * 2,
	PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
if (data == MAP_FAILED) {
	/* handle error; Chapter 9 checks every mmap */
	return -1;
}
struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, frame_size * 2);
close(fd);

struct wl_buffer *buf0 = wl_shm_pool_create_buffer(pool, 0,
	width, height, stride, WL_SHM_FORMAT_XRGB8888);
struct wl_buffer *buf1 = wl_shm_pool_create_buffer(pool, frame_size,
	width, height, stride, WL_SHM_FORMAT_XRGB8888);

uint32_t *px = (uint32_t *)(data + 0);
for (int y = 0; y < height; ++y)
	for (int x = 0; x < width; ++x)
		px[y * width + x] = ((x / 32 + y / 32) % 2)
			? 0xFF202020 : 0xFFC0C0C0;

wl_shm_pool_destroy(pool); /* safe immediately: buffers keep their own pool reference */

wl_surface_attach(surface, buf0, 0, 0);
wl_surface_damage(surface, 0, 0, width, height);
wl_surface_commit(surface);
```

Damaged means needs redraw: `damage` tells the compositor which surface-coordinate rectangles must be repainted on the next commit. To mark everything dirty, use the full-size rectangle above, or the `UINT32_MAX` idiom `wl_surface_damage(surface, 0, 0, UINT32_MAX, UINT32_MAX)` which covers any size without knowing width and height. `wl_surface.damage` takes surface coordinates; `wl_surface.damage_buffer` (surface v4 and later) takes buffer coordinates instead. The two differ under buffer scale and transform, which is why `damage_buffer`, buffer scale, and regions each get in-depth coverage in a later chapter.

Next frame, draw into `buf1` and attach it. Ping-ponging avoids tearing. To hide, attach `NULL`:

```c
wl_surface_attach(surface, NULL, 0, 0);
wl_surface_commit(surface);
```

Lifecycle (damage alone triggers nothing; only `commit` publishes pending state):

```
alloc -> mmap -> pool -> buffer -> attach -> damage -> commit -> release -> destroy
  fd     MAP_SHARED  create_pool  create_buffer  pending  pending  ^ compositor reads here
```

`wl_buffer.release` returns the loan. Until it fires, the compositor may still be reading. Optimization: if `release` for the current buffer arrives before the next frame callback in the same commit cycle, the client may reuse that buffer immediately without double-buffering; double-buffering is only required when the compositor still holds the buffer.

> **Misconception: "The buffer holds pixels I can rewrite anytime."**
> No. After commit the bytes are on loan, possibly scanned out by the GPU or sent over the network. Rewriting before `release` tears or corrupts. Draw, lend, wait for `release`, then reuse. That is why double-buffering exists: one slot displays while you paint the other.

## 8.7 Server side: reading SHM

Compositors initialise SHM once, then inspect committed buffers. The buffer arrives as a request-handler parameter; there is no accessor to fetch it from the surface:

```c
wl_display_init_shm(display);
wl_display_add_shm_format(display, WL_SHM_FORMAT_XRGB8888);
wl_display_add_shm_format(display, WL_SHM_FORMAT_ARGB8888);

/* attach request handler: the buffer arrives here as a parameter */
static void
surface_attach(struct wl_client *client, struct wl_resource *surface_resource,
		struct wl_resource *buffer, int32_t x, int32_t y)
{
	/* stash buffer as pending state; on wl_surface.commit, inspect it: */
	struct wl_shm_buffer *sb = wl_shm_buffer_get(buffer);
	if (sb) {
		wl_shm_buffer_begin_access(sb);
		uint8_t *pixels = wl_shm_buffer_get_data(sb);
		int32_t w = wl_shm_buffer_get_width(sb);
		int32_t h = wl_shm_buffer_get_height(sb);
		int32_t stride = wl_shm_buffer_get_stride(sb);
		uint32_t fmt = wl_shm_buffer_get_format(sb);
		/* copy or upload pixels inside the critical section */
		wl_shm_buffer_end_access(sb);
	}
}
```

Borrow, do not free: do not touch the `get_data` pointer outside the `begin_access`/`end_access` critical section, and do not free it. Its lifetime is tied to the buffer and its pool; if the compositor must retain pixels beyond commit handling, take a reference with `wl_shm_buffer_ref` and release it later with `wl_shm_buffer_unref`. `NULL` from `wl_shm_buffer_get` means this is not an SHM buffer at all, likely dmabuf, and needs the GPU path.

## 8.8 dmabuf and the EGL shortcut

SHM wastes GPU work: render on GPU, read back to CPU RAM, upload to GPU again for scanout. It also cannot express multi-plane YUV, modifiers, or hardware overlays. `linux-dmabuf` passes GPU handles directly, but its full handshake (`get_default_feedback`, tranches, `create_params`, planes in `linux-dmabuf-v1.xml`) is heavy. For protocol detail see that XML plus `src/egl/drivers/dri2/platform_wayland.c` in Mesa and `types/wlr_linux_dmabuf_v1.c` in wlroots.

Applications usually let EGL do it:

1. `eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_KHR, wl_display, NULL)`.
2. Choose a config with `EGL_WINDOW_BIT`.
3. `wl_egl_window_create(surface, width, height)`.
4. `eglCreatePlatformWindowSurfaceEXT(display, config, wl_egl_window, NULL)` (EGL 1.5 core renamed it to `eglCreatePlatformWindowSurface` without the `EXT` suffix; either name is correct for its EGL version).
5. Render, then `eglSwapBuffers` (attaches and commits a dmabuf buffer).
6. On resize, `wl_egl_window_resize`.

Use SHM for cursors, tooltips, and software widgets; use EGL/dmabuf for video, games, and GPU textures.

## 8.9 Roles: one surface, one job

A role assigns meaning: `xdg_surface` is a window, `wl_subsurface` a child, cursor protocol a pointer image, `zwlr_layer_surface_v1` a panel or wallpaper. Each surface takes exactly one role, once. Attaching a second role, e.g. making the same `wl_surface` both a window and a cursor, is a protocol error and the client is disconnected.

Roles also gate when buffers are legal: attaching a buffer before the role's first configure is a client error. For `xdg_surface` this raises the `unconfigured_buffer` fatal error; other role protocols have equivalent configure-before-attach rules. This is why surfaces, buffers, and roles are inseparable: a committed buffer is only meaningful inside a configured role. `unconfigured_buffer`, like `damage_buffer` and buffer scale, is covered in depth in the roles/xdg-shell chapter.

Create one `wl_surface` per window, cursor, or layer. To show identical pixels twice, attach the same `wl_buffer` to two surfaces; never reuse the surface.

## Recap

- `wl_surface` is a blank endpoint (top-left origin); `wl_buffer` is opaque storage. Surface needs buffer plus role plus commit.
- `wl_compositor` v4 gives surfaces/regions; `wl_shm` v1 gives software pixels. Only ARGB8888/XRGB8888 guaranteed; prefer XRGB8888; watch little-endian order.
- Helpers use `shm_open`/`shm_unlink`/`ftruncate` with `EINTR`/`EEXIST` retries; pools obey `offset + stride * height`.
- Present via attach, damage, commit; hide via null attach; rewrite only after `release`.
- Servers use `init_shm`/`add_shm_format` and `begin_access`/`get_data`/`end_access` without freeing.
- dmabuf avoids the GPU to CPU to GPU round trip; EGL wraps it.
- One role per surface; a second role is fatal.

## Exercises

1. **Single-pixel pool.** Build a 1x1 `XRGB8888` pool. Compute stride, frame, and pool size by hand, fill it red, attach, damage the 1x1 region, and commit. Confirm under `WAYLAND_DEBUG=1` that no error fires, then explain why a 1-byte pool fails.
2. **WAYLAND_DEBUG attach trace.** Run an SHM client as `WAYLAND_DEBUG=1 ./client`. Match `wl_surface.attach`, `damage`, and `commit` lines to the lifecycle diagram, note when `wl_buffer.release` arrives relative to your next draw, and state what breaks if `damage` is omitted.
