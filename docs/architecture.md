# Architecture

Tour of the tree as built.
Layers: `main.c` drives an opaque
app (`include/flint/flint.h`,
`src/flint.c`) that owns Wayland
(`src/wl.h`, `src/wl.c`) plus
Vulkan (`src/vk.h`, `src/vk.c`).
Retained widgets live in C++
(`include/flint/ui.h`,
`src/ui.cpp`) behind a C ABI.
`Makefile` builds all of it.
See `docs/backend.md` for guide
chapter mapping, `docs/ui-model.md`
for the target model,
`docs/decisions.md` for rationale,
`docs/invariants.md` for scope.

## main.c: entry

Parses `--frames N` in
`parse_frames`. No args means run
until close. `N > 0` bounds the
loop. Anything else prints
`usage: flint [--frames N]` or
`frames must be N > 0` to stderr
and returns 1.

Creates one app with
`fl_app_create`. Maps
`FL_ERR_NODISPLAY` to a headless
skip: prints `no display,
skipping` and returns 0. Any
other error prints `failed to
create app` and returns 1.

Loop: `while (frames != 0 &&
fl_app_frame(app))` with
decrement only when bounded.
Then `fl_app_destroy` and
return 0. No other logic lives
here. Frame pacing, resize, and
exit all live in `fl_app_frame`.

## flint.h and flint.c: app object

`fl_app_t` is an opaque pointer
to `struct fl_app`. The struct
is defined only in `src/flint.c`
and holds a `struct Wl` plus a
`struct Vk` by value. Callers
never dereference it. Layout can
change without breaking ABI.

`fl_result_t` codes: `FL_OK` 0,
`FL_ERR_NODISPLAY` 1,
`FL_ERR_NOPROTOCOL` 2 (defined,
never returned today),
`FL_ERR_NOMEM` 3,
`FL_ERR_NOVULKAN` 4,
`FL_ERR_NOSYS` 5 (not
implemented). See cross-cutting
below for meanings.

`fl_app_create` takes a null
check, zeroes the malloc, then
`wl_init` followed by `vk_init`.
Failure unwind: `wl_init` fail
frees the app and returns
`NODISPLAY`. `vk_init` fail
calls `vk_finish` plus
`wl_finish`, frees the app, and
returns `NOVULKAN`. Success sets
`*out` (`NULL` on every failure
path) and returns `FL_OK`.

`fl_app_frame` pumps one frame
and returns false to exit. Order
is fixed: `wl_dispatch`, then
check `wl.closed`, then if
`wl.resized` call
`vk_swapchain_create` and clear
the flag, then `vk_draw_frame`
with a `resize_needed` out
param, then a second
`vk_swapchain_create` if the
draw reported stale. Null app,
dispatch failure, close flag, or
unrecoverable draw error all
return false. `OUT_OF_DATE` and
`SUBOPTIMAL` are recoverable and
stay true with a recreate.

`fl_app_destroy` is a null no-op.
Order: `vk_finish`, then
`wl_finish`, then free. Vulkan
tears down before Wayland because
the surface outlives the device.

## wl.h and wl.c: Wayland

`struct Wl` in `src/wl.h` holds
`display`, `registry`,
`compositor`, `wm_base`,
`surface`, `xdg_surface`,
`xdg_toplevel`, plus `width` and
`height`, plus three flags:
`configured` (first ack done,
present is legal), `closed`
(toplevel close seen, loop must
exit), `resized` (compositor
asked for a new size, renderer
must recreate).

`wl_init` order: memset zero,
default size 1280 by 720,
`wl_display_connect(NULL)` (uses
`WAYLAND_DISPLAY` or the
default), `wl_display_get_registry`,
add `registry_listener`, one
`wl_display_roundtrip` to deliver
the global burst. `registry_global`
binds `wl_compositor` version 4
and `xdg_wm_base` version 1 by
interface name compare and
ignores the rest.
`registry_global_remove` is an
empty stub required by the
listener shape.

Then null check both globals,
add the `ping` listener,
`wl_compositor_create_surface`,
`xdg_wm_base_get_xdg_surface`,
add the surface listener,
`xdg_surface_get_toplevel`, add
the toplevel listener, set title
`Flint` and app id `flint`,
`wl_surface_commit` with no
buffer to request the first
configure.

Handshake: `wm_base_ping`
replies with `pong` so the
compositor never marks the window
unresponsive.
`xdg_surface_configure` acks the
serial (mandatory, before any
later configure) and sets
`configured`.
`xdg_toplevel_configure` treats
zero width or height as no
preference and otherwise stores
the size and sets `resized` only
when it changed.
`xdg_toplevel_close` sets `closed`.
The main loop observes the flags.
Nothing here tears down directly.

Guarded wait: loop
`wl_display_roundtrip` until
`configured`, capped at
`WL_CONFIGURE_ROUNDTRIP_LIMIT`
(100). A failing roundtrip or a
silent compositor prints to
stderr and unwinds. Presenting
before this ack violates the
protocol, so init does not return
until it arrives.

Every failure path prints to
stderr, calls `wl_finish`, and
returns false. Listeners are
`static const` because Wayland
keeps the pointer, not a copy.

`wl_dispatch` is one nonblocking
round per render iteration.
Sequence: drain queued events
(`prepare_read` loop with
`dispatch_pending`), `flush`
(`EAGAIN` tolerated), zero
timeout `poll` on
`wl_display_get_fd`, then
`cancel_read` when idle or
`read_events` when readable,
then a final `dispatch_pending`.
Any I/O error returns false.
Otherwise returns `!closed`, so a
close request also ends the loop.

`wl_finish` destroys in reverse
creation order: toplevel, xdg
surface, surface, `wm_base`,
compositor, registry, display.
Each pointer is nulled, so repeat
calls and partial inits are safe.

## vk.h and vk.c: Vulkan

`struct Vk` in `src/vk.h` holds
`inst`, debug `dbg` (debug builds
only), `phys`, `dev`, `queue`,
`qfam`, Wayland `surf`,
`swapchain`, `format`, `extent`,
`image_count` plus
`images[VK_MAX_SWAPCHAIN_IMAGES]`,
one `cmd_pool` plus
`cmd_bufs` per image, one
`image_available` semaphore, one
`render_finished` semaphore per
image slot, one `in_flight`
fence. `VK_MAX_SWAPCHAIN_IMAGES`
is 8. All variable count queries
(devices, families, formats,
images) clamp to fixed stack
arrays of 8 and treat
`VK_INCOMPLETE` as success after
clamp.

`vk_init` needs a connected `Wl`
for `display` and `surface`.
Steps: instance with
`VK_KHR_surface` plus
`VK_KHR_wayland_surface` (plus
`EXT_debug_utils` in debug),
`Flint` 1.0.0 app and engine
metadata against API 1.0, then
Wayland surface via
`vkCreateWaylandSurfaceKHR`,
then two call physical device
enum, then queue family scan for
one family with both
`GRAPHICS_BIT` and surface
support against the real surface
(no ownership transfer), then
logical device with
`VK_KHR_swapchain`, then
`vkGetDeviceQueue`, then command
pool with
`RESET_COMMAND_BUFFER_BIT`, then
sync, then `vk_swapchain_create`.
`vk.h` requires calling
`vk_finish` after any return.

Validation path (debug only,
`#ifndef NDEBUG`): layer
`VK_LAYER_KHRONOS_validation`,
messenger severity warning plus
error, types general plus
validation plus performance,
callback `vk_debug_cb` to stderr
returning false. The create info
rides `pNext` so instance
creation itself is checked. The
persistent messenger loads
`vkCreateDebugUtilsMessengerEXT`
by name via
`vkGetInstanceProcAddr`. The
`dbg` field compiles out in
release.

Sync shape: one frame in flight,
so one `image_available`
semaphore plus one `in_flight`
fence starting signaled. But one
`render_finished` semaphore per
image slot. Reason: the fence
tracks submit, not present
consumption, so a shared signal
could be re-signaled while a
present still holds it. Each
frame signals only its own
`render_finished[image_index]`.
Partial semaphore creation
failure unwinds the slots made so
far inline. Other mid init
failures return false and rely on
the caller `vk_finish`.

`vk_swapchain_create` doubles as
first create and resize recreate.
It calls `vk_swapchain_destroy`
first. Then surface caps, two
call surface formats (prefer
`B8G8R8A8_UNORM` sRGB nonlinear,
else first entry), extent from
`currentExtent` unless
`0xFFFFFFFF` (compositor defers),
in which case `wl` width and
height clamped into `min` and
`max`, count `minImageCount + 1`
capped at `maxImageCount` (0 is
no maximum), `EXCLUSIVE` sharing,
`currentTransform`,
`OPAQUE` alpha, `FIFO` present
(the only required mode),
`clipped` true, usage
`COLOR_ATTACHMENT` plus
`TRANSFER_DST` so
`vkCmdClearColorImage` writes
with no render pass or pipeline.
Then `vkGetSwapchainImagesKHR`
and one primary command buffer
per image, re-recorded each use.

`vk_swapchain_destroy` returns
early when `dev` is null.
Otherwise `vkDeviceWaitIdle`,
frees the command buffers, zeroes
them, clears `image_count`, and
destroys the swapchain. Leaves
device, surface, and instance
intact for recreation.

`vk_draw_frame` acquires, clears,
and presents. Steps: wait
`in_flight` forever, acquire with
timeout infinite on
`image_available`,
`OUT_OF_DATE` means set
`resize_needed` true and return
true (nothing submitted),
`SUBOPTIMAL` continues, reset
fence, reset and begin the image
command buffer
(`ONE_TIME_SUBMIT`), barrier
`UNDEFINED` to `TRANSFER_DST`,
clear to `0.05, 0.05, 0.08,
1.0`, barrier `TRANSFER_DST` to
`PRESENT_SRC_KHR`, end, submit
waiting on `image_available` at
`TRANSFER` stage and signaling
`render_finished[image_index]`
with `in_flight` as the submit
fence, then present waiting on
that same per image semaphore.
Present `OUT_OF_DATE` or
`SUBOPTIMAL` sets `resize_needed`
true and returns true. Any other
`VkResult` prints the code to
stderr and returns false.

`vk_finish` order: `WaitIdle` if
`dev` exists, swapchain destroy,
fence, each per image semaphore,
acquire semaphore, command pool,
debug messenger before the
surface (debug only, still logs
surface teardown), surface,
device (plus queue null), then
instance, then `phys` null. All
handles null after destroy.

## ui.h and ui.cpp: retained tree

`fl_widget_t` is opaque, same
pattern as `fl_app_t`. The
`struct fl_widget` lives only in
`src/ui.cpp`. Only the handle
crosses the C boundary. Public
line height
`FL_WIDGET_LINE_H` is 20.

Internals are RAII C++:
`Kind` (`Root`, `Label`, `Box`,
`Panel`, `Viewport`, `Table`),
`std::string text`,
`std::vector<std::unique_ptr<fl_widget>>`
children (owning), raw `parent`
(non owning), `x`, `y`, `w`,
`h`, `horizontal`, `spacing`,
`rows`, `is_root`. `delete` on a
root cascades through
`unique_ptr` and frees the
subtree. `delete nullptr` is a
no-op, so destroy of `NULL` is
safe.

C boundary rules: all public
functions are `extern "C"`,
signatures use only C types
(`bool`, `int`, `unsigned`,
`char*`, handles), never
`std::string`, `vector`,
references, or templates. Every
function that can allocate wraps
its body in try and catch all and
returns `FL_ERR_NOMEM`. No
exception crosses. Stubs return
`FL_ERR_NOSYS` instead.

Constructors: `root_create(w,
h)` at `(0, 0)` marked root,
`label_create` copies text (`NULL`
is empty) as a zero bounds leaf,
`box_create(horizontal)`,
`panel_create` copies title
(`NULL` is untitled),
`viewport_create` as a leaf,
`table_create(rows)`. Each checks
null `out` and maps alloc failure
to `NOMEM`.

`fl_widget_append` moves `child`
under `parent`. It rejects null
args, a child that already has a
parent, and cycles via
`is_descendant(parent, child)`.
It calls `reserve` before
`emplace_back` so a growth
failure leaves ownership with
the caller. On success the parent
owns the child and the caller
must not free it. On failure the
caller keeps it.

`label_set_text` swaps in a copy.
`NULL` clears to empty.
`widget_bounds` copies out `x`,
`y`, `w`, `h` and skips any null
out pointer. `box_set_spacing`
records the pixel gap and rejects
null or non box widgets.

Layout: `fl_widget_layout(root)`
calls `layout_at(root, root->x,
root->y, root->w)`. `leaf_height`
gives childless size: root keeps
`h`, panel one line, viewport
200 (`kViewportH`), table
`rows` times one line (one line
when `rows` is 0), box 0, label
one line.

`layout_at(w, x, y, wd)` sets
position and width, then per
kind: empty child list sizes to
`leaf_height` except the root,
which keeps `h`. Viewport and
table lay out children stacked at
full width but keep their own
leaf height. Panel starts
children below one title line and
sizes to title plus stacked
children except the root.
Horizontal box splits `wd` minus
`spacing * (n - 1)` evenly
(remainder one pixel to the first
children, zero avail when gaps
exceed width), takes the tallest
child height, advances by child
width plus spacing. All other
containers (root, vertical box,
label with children) stack full
width rows top to bottom, adding
`spacing` between box children
only. Non root containers size to
fit (`h = cursor` or `maxh`).
Root always keeps its created
height and returns it.

Stubs and what each waits on:
`fl_panel_set_dock` waits on the
docking design (panel
relationships in `ui-model.md`).
`fl_viewport_render` waits on the
GPU viewport path through the
Vulkan backend (viewport
separation in `ui-model.md`).
`fl_table_set_cell` waits on cell
storage plus virtualized rows
(runtime owns state in
`ui-model.md`). All three ignore
args and return `FL_ERR_NOSYS`.

## Makefile: build

Codegen: `wayland-scanner`
`client-header` and
`private-code` turn
`/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml`
into
`generated/xdg-shell-client-protocol.h`
and `generated/xdg-shell-protocol.c`.
C flags carry `-Igenerated`.

Strict dialects: C as C23 and C++
as C++23, both `-pedantic -Wall
-Wextra -Werror -O2`, plus
`-Wunused-result` for C. No GNU
extensions. `CC ?= cc`,
`CXX ?= c++`.

Sources: explicit `C_SRCS`
(`main.c`, `src/vk.c`,
`src/wl.c`, `src/flint.c`, plus
the generated protocol source).
`CXX_SRCS` uses
`$(wildcard src/ui.cpp)` so make
works before the file lands.
`pkg-config` supplies
`wayland-client` and `vulkan`
cflags and libs when present.

Rules: C objects depend on the
generated header and build with
`$(CC)`. C++ objects build with
`$(CXX)` and `-Iinclude -I.`.
Link always uses `$(CXX)` so the
C++ object resolves libstdc++
with no extra flags into
`bin/flint`.

Targets: `all` builds
`bin/flint`. `check` builds then
runs `./bin/flint --frames 120`.
`clean` removes `main.o`,
`src/*.o`, `bin`, `dist`,
`generated`. `.PHONY` lists
`all check clean`.

## Cross-cutting

Single threaded. No thread,
mutex, or queue family beyond the
one graphics plus present queue
in `src/`, `main.c`, or headers.
Wayland fd is polled only inside
`wl_dispatch` with timeout 0 so
rendering paces the loop. One
frame in flight means wait, use,
and reset of the same fence is
safe. UI tree, `Wl`, and `Vk`
are not thread safe. Call all of
it from one thread.

Error discipline. `true` and
`false` stay inside `wl` and
`vk` with details on stderr.
Codes cross at `fl_app` and the
UI boundary. `NOMEM` is alloc
failure, null args, UI misuse
(taken child, cycle, wrong kind),
or any caught C++ exception.
`NODISPLAY` is `wl_init` failure
(missing display, missing
compositor or `wm_base` global,
no configure). `NOVULKAN` is
`vk_init` or swapchain setup
failure after Wayland connected.
`NOSYS` is structure present,
behavior future (dock, viewport
render, table cell).
`NOPROTOCOL` is reserved and
unused. `main.c` treats only
`NODISPLAY` as a pass with exit
0. All else exits 1.

C and C++ boundary. Opaque
handles only (`fl_app_t`,
`fl_widget_t`). `extern "C"` on
every public UI function. No C++
headers from `src/vk.c`,
`src/wl.c`, or `src/flint.c`. No
`std` types in `ui.h`. Catch all
in `ui.cpp`. Link with `$(CXX)`.

## Not built yet

Input: no seat, pointer, or
keyboard code, no `frame`
callbacks, damage, regions,
subsurfaces, or scale handling.
See not yet list in
`docs/backend.md` and input plus
state in `docs/ui-model.md`.

Text: labels store a `std::string`
and size to one line. No shaping,
wrapping, editing, or render.
Runtime owns text in
`docs/ui-model.md`.

Docking: `fl_panel_set_dock` is
`NOSYS`. Model is panels plus
docking and workspaces in
`docs/ui-model.md`.

Viewport render:
`fl_viewport_render` is `NOSYS`.
Only clear plus present runs
today. Model is a dedicated GPU
viewport path with separate
budgets in `docs/ui-model.md`.

Table cells: `fl_table_set_cell`
is `NOSYS`. Only row count sizes
layout. Model is virtualized
tables in `docs/ui-model.md`.

GPU work: no shaders,
descriptors, pipelines, passes,
framebuffers, draw calls, or
device memory management. Default
allocator only. See not yet list
in `docs/backend.md`.
