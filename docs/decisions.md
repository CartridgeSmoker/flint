# Decisions

## C23 for C code

All C sources (`main.c`, `src/vk.c`, `src/wl.c`, `src/flint.c`, generated `xdg-shell-protocol.c`) build with `-std=c23` (see `Makefile` `CFLAGS`). Rationale: the backends use modern zero-initializer and designator idioms throughout (`Vk...CreateInfo = {0}` plus field assignment), and a single current standard keeps compiler behavior uniform across `cc` variants instead of depending on GNU-default drift.

## C++23 confined to the retained UI layer with a pure-C ABI boundary

C++ is allowed only in the retained UI layer (`src/ui.cpp`, public header `include/flint/ui.h`). Rationale: the UI model in `docs/ui-model.md` needs RAII ownership for retained widgets (`std::string`, `std::vector`, `std::unique_ptr` inside `struct fl_widget`), which C++23 expresses directly, while the windowing and Vulkan backends stay C so they keep a stable ABI and remain callable from any C toolchain. Boundary rules: opaque handles (`fl_widget_t`, same pattern as `fl_app_t` in `include/flint/flint.h`), all public UI functions declared `extern "C"`, no C++ types (no `std::string`, `std::vector`, references, or templates) in public signatures, no exceptions crossing the boundary (catch everything inside `src/ui.cpp` and return an error code), and backends stay C (never include C++ headers from `src/vk.c`, `src/wl.c`, or `src/flint.c`).

## FL_ERR_NOVULKAN addition

`include/flint/flint.h` defines `FL_ERR_NOVULKAN = 4` alongside `FL_ERR_NODISPLAY`, `FL_ERR_NOPROTOCOL`, and `FL_ERR_NOMEM`, and `src/flint.c` `fl_app_create` returns it when `vk_init` fails after `wl_init` succeeded (tearing down both via `vk_finish` plus `wl_finish`). Rationale: Wayland-connectivity failure and Vulkan-setup failure have different remedies (missing compositor versus missing driver or GPU), and `main.c` already treats `FL_ERR_NODISPLAY` as a headless skip while any other code, including `FL_ERR_NOVULKAN`, is a hard failure with exit 1, so the distinction must be representable at the API level.

## Render-loop ownership in fl_app_frame

The frame loop is owned by `fl_app_frame` in `src/flint.c`, which does one `wl_dispatch`, then one swapchain check, then one `vk_draw_frame`, and returns false when the app should exit (null app, dispatch failure, close flag, or unrecoverable draw error). Rationale: keeping the pump-then-draw sequence in one place means `main.c` stays a three-line loop (create, frame until false, destroy), resize handling (`wl.resized` flag and `resize_needed` out-param both funnel into `vk_swapchain_create`) cannot be skipped by callers, and the UI layer later gets a single per-frame hook instead of coordinating two backends itself.

## Validation layers in debug builds

`src/vk.c` `vk_init` and `vk_finish` enable `VK_LAYER_KHRONOS_validation` plus an `EXT_debug_utils` messenger logging warnings and errors to stderr only under `#ifndef NDEBUG`; release builds (`-DNDEBUG`) compile both out, including the `dbg` field in `struct Vk` (`src/vk.h`). Rationale: validation catches misuse (wrong layouts, stale swapchains, teardown order) during development with zero release cost, and gating on the standard `NDEBUG` macro means plain `make` (no flags) validates while `make CFLAGS=-DNDEBUG` style release builds ship clean, with no runtime flag to forget.

## Fixed-array clamps

Variable-count Vulkan queries (physical devices, queue families, surface formats, swapchain images) are stored in fixed stack arrays of 8 (`VK_MAX_SWAPCHAIN_IMAGES` is 8 in `src/vk.h`) and any count above that is clamped, with `VK_INCOMPLETE` explicitly treated as success-after-clamp. Rationale: every driver seen in practice returns far fewer than 8 entries, so a bounded array avoids heap allocation and its failure paths in setup code, while the clamp plus `VK_INCOMPLETE` handling keeps the code correct (uses the first 8) rather than overflowing or failing hard if a future driver ever reports more.

## C++ stub rule with FL_ERR_NOSYS

New UI surface lands structure-complete before behavior is ready: `include/flint/ui.h` declares docking, viewport render, and table cell entry points now, and the `src/ui.cpp` stubs return `FL_ERR_NOSYS = 5` ("operation not implemented", appended without renumbering in `include/flint/flint.h`) until real behavior lands. Rationale: callers can build retained trees against the full shape early while layout geometry stays honest, and NOSYS separates "not yet built" from allocation or backend failures.

## Internal header guard prefix

Module file names stay short (`src/vk.h`, `src/wl.h`), but their include guards must carry the `FLINT_` prefix, so `VK_H` became `FLINT_VK_H` and `WL_H` became `FLINT_WL_H`. Rationale: generic guard names risk collision once any third party header uses the same two letters, while the prefixed form keeps the local file names unchanged and matches the existing `FLINT_FLINT_H` and `FLINT_UI_H` style.

## Flat src/ until the second backend or tenth file

`src/` stays flat (`flint.c`, `vk.c`, `wl.c`, `ui.cpp` plus local headers) with no `core/`, `backend/`, or `ui/` subfolders. Rationale: seven files need no navigation aid, and premature nesting churns include paths and the `Makefile` for zero benefit. Split when either trigger hits: a second platform backend lands, or `src/` passes ten files. The pre-decided shape then is `src/core/` (app object), `src/backend/` (windowing and Vulkan), `src/ui/` (widgets and layout), with internal headers moving alongside their sources.
