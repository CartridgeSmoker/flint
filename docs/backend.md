# Backend map: guide chapters to code

This file maps the tutorial chapters in `docs/wayland/` and
`docs/vulkan/` to the files that implement them. Unlisted chapters
are background not yet exercised by the tree.

## Wayland: `src/wl.c` and `src/wl.h`

Implemented in `src/wl.c` (`wl_init`, `wl_dispatch`, `wl_finish`)
with state in `struct Wl` (`src/wl.h`).

- `docs/wayland/06-display-eventloop.md` (displays and event loops):
  `wl_init` calls `wl_display_connect(NULL)` and
  `wl_display_roundtrip` for setup; `wl_dispatch` implements the
  steady-state half of the chapter with a render-loop adaptation:
  `prepare_read` / `flush` / zero-timeout `poll` on
  `wl_display_get_fd` / `read_events` / `dispatch_pending`, instead
  of the chapter's blocking `poll(..., -1)` plus `dispatch`.
- `docs/wayland/07-registry.md` (registry): `registry_global`
  binds `wl_compositor` (version 4) and `xdg_wm_base` (version 1)
  by interface-name compare and ignores the rest;
  `registry_global_remove` is a stub; one `wl_display_roundtrip`
  delivers the burst before the null checks.
- `docs/wayland/08-surfaces-shm.md` (surfaces): only the surface
  half applies. `wl_init` calls `wl_compositor_create_surface`.
  The SHM sections (pools, `wl_shm_create_pool`, attach/damage of
  software buffers) are background: Flint never creates a `wl_shm`
  buffer and presents through the Vulkan swapchain in `src/vk.c`
  instead.
- `docs/wayland/09-xdg-basics.md` (first window): `wl_init` wraps
  the surface with `xdg_wm_base_get_xdg_surface`, takes the
  toplevel role with `xdg_surface_get_toplevel`, sets title
  (`xdg_toplevel_set_title`) and app id
  (`xdg_toplevel_set_app_id`), answers `ping` with `pong` in
  `wm_base_ping`, commits once with no buffer to request the first
  configure, then blocks in `roundtrip` until `xdg_surface_configure`
  acks the first serial (guarded by
  `WL_CONFIGURE_ROUNDTRIP_LIMIT`, 100).
- `docs/wayland/13-xdg-in-depth.md` (configure/close for real):
  `xdg_toplevel_configure` treats a zero width or height as "no
  preference" and otherwise stores the size and sets `resized`;
  `xdg_toplevel_close` sets `closed` for the main loop to observe.
  Nothing may present before `configured` is true.
- Not yet implemented: `docs/wayland/10-surfaces-in-depth.md`
  (no `frame` callbacks, damage, regions, subsurfaces, or scale
  handling), `docs/wayland/11-input.md` and
  `docs/wayland/12-input-example.md` (no seat/pointer/keyboard
  code), `docs/wayland/14-clipboard-dnd.md`,
  `docs/wayland/15-extensions.md`.
- Protocol codegen (`docs/wayland/05-libwayland.md` plus
  `docs/wayland/16-writing-extensions.md` mechanics): `Makefile`
  runs `wayland-scanner client-header` and `private-code` on
  `stable/xdg-shell/xdg-shell.xml` into
  `generated/xdg-shell-client-protocol.h` and
  `generated/xdg-shell-protocol.c`, built with `-Igenerated`.

## Vulkan: `src/vk.c` and `src/vk.h`

Implemented in `src/vk.c` (`vk_init`, `vk_swapchain_create`,
`vk_swapchain_destroy`, `vk_draw_frame`, `vk_finish`) with state
in `struct Vk` (`src/vk.h`).

- `docs/vulkan/01-overview.md` (instance/device/queue hierarchy):
  overall shape of `vk_init`: `VkInstance`, physical device,
  logical device, queue.
- `docs/vulkan/02-device-setup.md` (setup order): instance with
  `VK_KHR_surface` plus `VK_KHR_wayland_surface` extensions,
  two-call `vkEnumeratePhysicalDevices`, queue-family scan for a
  graphics-capable family, `vkCreateDevice` with
  `VK_KHR_swapchain`, then `vkGetDeviceQueue`.
- `docs/vulkan/03-debugging.md` (validation): debug-only
  `VK_LAYER_KHRONOS_validation` layer plus `EXT_debug_utils`
  messenger (`vk_debug_cb` to stderr), created via
  `vkGetInstanceProcAddr` and destroyed in `vk_finish` before the
  surface. Gated by `#ifndef NDEBUG`; see `Makefile` (no `-DNDEBUG`
  by default, so plain `make` validates).
- `docs/vulkan/05-commands-queues.md` (queues and command
  buffers): one command pool plus one primary buffer per swapchain
  image, allocated up front and re-recorded every frame.
- `docs/vulkan/06-moving-data.md` (barriers and clears): each
  frame records `UNDEFINED -> TRANSFER_DST_OPTIMAL`, then
  `vkCmdClearColorImage`, then `TRANSFER_DST_OPTIMAL ->
  PRESENT_SRC_KHR` via `vkCmdPipelineBarrier`. No pipeline or
  render pass exists yet; swapchain images carry
  `TRANSFER_DST_BIT`.
- `docs/vulkan/07-presentation.md` (WSI and swapchain): surface
  creation with `vkCreateWaylandSurfaceKHR` from `wl.display` /
  `wl.surface` before device selection so
  `vkGetPhysicalDeviceSurfaceSupportKHR` tests the real surface;
  combined graphics-plus-present family (no ownership transfer);
  swapchain from surface capabilities (extent, `minImageCount + 1`
  clamped to `maxImageCount`), preferred `B8G8R8A8_UNORM` sRGB
  format, `FIFO` present mode; per-frame acquire, record, submit,
  present with `OUT_OF_DATE` / `SUBOPTIMAL` mapped to a
  `resize_needed` out-param.
- `docs/vulkan/15-synchronization.md` (fences and semaphores):
  exactly one frame in flight, so one `image_available` semaphore,
  one `render_finished` semaphore, and one signaled-start
  `in_flight` fence, waited and reset each `vk_draw_frame`.
- Not yet implemented: `docs/vulkan/04-memory.md` (default
  allocator, no device-memory management),
  `docs/vulkan/08-shaders.md`,
  `docs/vulkan/09-descriptors-bindless.md`,
  `docs/vulkan/10-graphics-pipelines.md`,
  `docs/vulkan/11-passes-framebuffers.md` (clear without a pass),
  `docs/vulkan/12-drawing.md` (no draw calls).

## App glue: `src/flint.c`, `include/flint/flint.h`, `main.c`

- `docs/vulkan/17-engine-arch.md` (loop structure): `src/flint.c`
  owns the per-frame sequence in `fl_app_frame` (dispatch Wayland,
  recreate swapchain on `resized`, draw, recreate on
  `resize_needed`), over the opaque `fl_app_t` from
  `include/flint/flint.h`. `main.c` is create, frame-until-false,
  destroy, with `FL_ERR_NODISPLAY` mapped to a headless skip
  (exit 0) and all other errors to exit 1.
- Contrast with `docs/wayland/06-display-eventloop.md`: the guide
  chapter blocks in `dispatch` between frames; Flint inverts this
  and polls the display with a zero timeout inside its own render
  loop (`wl_dispatch` in `src/wl.c`, driven by `fl_app_frame`),
  so rendering paces the loop rather than input wakeups.
