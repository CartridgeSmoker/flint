# Chapter 15: Protocol Extensions: Vocabulary Beyond the Core

> **Objectives:** By the end of this chapter you will be able to explain core-as-grammar versus extensions-as-vocabulary; distinguish stable, staging, and unstable tiers; bind any extension with version capping and fallback; choose `wp_presentation`, `wp_viewporter`, `wp_fractional_scale_manager_v1`, `wp_single_pixel_buffer_manager_v1`, `zwp_pointer_constraints_v1`, `zwp_idle_inhibit_manager_v1`, layer-shell, decoration, activation, primary-selection, dmabuf, and explicit-sync by symptom.

## 15.1 Core Is Grammar, Extensions Are Vocabulary

Core Wayland - `wl_compositor`, `wl_surface`, `wl_seat`, `xdg_wm_base` - is grammar. It tells you how to form a sentence: create a surface, attach a buffer, receive input. It gives you almost no words.

Everything users notice is an extension: an extra registry global with its own requests and events. Nanosecond frame timing is `wp_presentation` (v2). Crisp 150% is `wp_viewporter` (v1) plus `wp_fractional_scale_manager_v1` (v1) plus `zxdg_output_manager_v1` (`zxdg_output_v1`; cap at v2/v3, see below). A dock above maximized windows is `zwlr_layer_shell_v1` - which is *not* in `wayland-protocols` at all; it lives in `wlr-protocols` (`unstable/wlr-layer-shell-unstable-v1.xml`). Zero-copy video is `zwp_linux_dmabuf_v1` (`stable/linux-dmabuf/linux-dmabuf-v1.xml` in `wayland-protocols`).

Compositors pick which vocabulary they speak. GNOME speaks a subset, KDE a larger one, Sway another. Do not memorize all 80 extensions. Learn the three tiers, learn bind-and-fallback once, and learn the dozen extensions that fix visible pain.

## 15.2 Shipping Tiers: Stable, Staging, Unstable

Verified against `wayland-protocols` 1.45 (Sept 2025). If you use a newer
release, re-check `stable/`, `staging/`, `unstable/` membership before
relying on the lists below - no promotion of the protocols named here has
happened as of 1.45.

**Stable: done, rely on it.** `stable/` holds only five protocols, and
stable protocols do not break. If a compositor advertises one, build on
it:

- `stable/presentation-time/presentation-time.xml` - `wp_presentation`, v2.
- `stable/viewporter/viewporter.xml` - `wp_viewporter`, v1.
- `stable/linux-dmabuf/linux-dmabuf-v1.xml` - `zwp_linux_dmabuf_v1`.
- `stable/tablet/tablet-v2.xml` - tablet support.
- `stable/xdg-shell/xdg-shell.xml` - `xdg_wm_base`.

**Staging: agreed, mostly settled, small revisions allowed.** Staging
means desktops agree on the design but reserve version bumps for fixes.
All of these live in `staging/` in `wayland-protocols`, not `stable/`:

- `staging/fractional-scale/fractional-scale-v1.xml` - `wp_fractional_scale_manager_v1`, v1.
- `staging/single-pixel-buffer/single-pixel-buffer-v1.xml` - `wp_single_pixel_buffer_manager_v1`, v1.
- `staging/xdg-activation/xdg-activation-v1.xml` - `xdg_activation_v1`, v1.
- `staging/fifo/fifo-v1.xml` - fifo-v1.
- `staging/ext-session-lock/ext-session-lock-v1.xml` - ext-session-lock.
- `staging/ext-data-control/ext-data-control-v1.xml` - `ext_data_control_manager_v1` (`ext_data_control_v1`, privileged; do not confuse with wlroots `wlr-data-control`).
- `staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml` - `wp_linux_drm_syncobj_manager_v1` / `wp_linux_drm_syncobj_v1`, the staging successor to unstable explicit-sync below.

Bind staging freely, but cap your version and check the changelog before
bumping. Staging bumps are additive by policy, but you must still gate
every `since=2+` request behind `bound_version >= N` - a cap without a
gate still kills you. Version 2 will add an event or request, not rename
the world - which is precisely why capping your version matters more,
not less.

**Unstable: lab notebook, gate on the exact version.** Unstable names
carry a `v1`/`unstable` suffix and live in `unstable/`. Authors can still
rename requests - not just events. Match the full name, demand the
version you tested, and keep a fallback:

- `unstable/pointer-constraints/pointer-constraints-unstable-v1.xml` - `zwp_pointer_constraints_v1`, v1.
- `unstable/relative-pointer/relative-pointer-unstable-v1.xml` - `zwp_relative_pointer_manager_v1`.
- `unstable/idle-inhibit/idle-inhibit-unstable-v1.xml` - `zwp_idle_inhibit_manager_v1`, v1.
- `unstable/xdg-decoration/xdg-decoration-unstable-v1.xml` - `zxdg_decoration_manager_v1`, v1 (see [Chapter 13, section 5](13-xdg-in-depth.md#5-decorations-who-draws-the-frame) for negotiation).
- `unstable/primary-selection/primary-selection-unstable-v1.xml` - `zwp_primary_selection_device_manager_v1` (see [Chapter 14](14-clipboard-dnd.md) for the core `wl_data_device` model it mirrors).
- `unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml` - `zwp_linux_explicit_synchronization_v1` (track as `wp_linux_explicit_synchronization`; staging successor is `wp_linux_drm_syncobj_v1` above).

`xdg-shell` shipped for years as `xdg_shell_unstable_v6` before
stabilizing with only mechanical renames. Others rot.

Defensive code against `v6` ported in an afternoon. Code that assumed `v6` was final died on stabilization. Write every unstable bind as if `v2` renames your favorite event.

## 15.3 Bind Mechanics Are Identical

Every extension is a registry global. Compare `interface`, take `min(yours, theirs)`, remember what you got, degrade gracefully when absent or old. Lower is always safe: binding 1 against a version-3 server gives you the version-1 subset. Calling a version-3 request on that bind is a fatal protocol error, so guard with `if (bound_version >= N)`.

```c
static void
global_add(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    struct app *a = data;

    if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        uint32_t want = version < 1 ? version : 1;
        a->viewporter = wl_registry_bind(registry, name,
            &wp_viewporter_interface, want);
        a->viewporter_version = want;
    } else if (strcmp(interface,
        zwp_pointer_constraints_v1_interface.name) == 0) {
        /* Unstable: pin the exact generation you tested.
         * Stable (e.g. wp_viewporter above) tolerates a missing
         * global with a warning; unstable MUST fall back to a
         * visible degraded path (hidden-cursor mode here). */
        uint32_t want = version < 1 ? version : 1;
        a->constraints = wl_registry_bind(registry, name,
            &zwp_pointer_constraints_v1_interface, want);
        a->constraints_version = want;
    }
}
```

Only the fallback differs. Missing stable `wp_viewporter` on a modern desktop deserves a warning. Missing unstable `zwp_pointer_constraints_v1` is normal: fall back to hidden-cursor mode and say why FPS look is degraded. Missing staging sits between: try, fall back, never crash.

> **Misconception callout: "Should I wait until everything I need is stable?"** No. Staging is dependable. Compositors ship staging years before stabilization, and bumps are additive by policy. Waiting for all-stable means shipping blurry, tearing, unfocused software while competitors ship crisp and smooth on the same staging protocols. Bind stable unconditionally, staging with a version cap and fallback, unstable with an exact-name gate plus visible fallback. Refusing staging does not make you robust. It makes you blurry.

## 15.4 Must-Knows by Pain

**presentation-time: stop guessing.** `wl_surface::frame` says "draw now" but not when the last commit hit glass. `wp_presentation` (`stable/presentation-time/presentation-time.xml`, v2) gives per-commit feedback: seconds plus nanoseconds stamp, refresh period, sequence counter, and flags for vsync, hardware clock (`hw_clock`), hardware completion (`hw_completion`), and zero-copy (`zero_copy`). Request `feedback()` after each commit, then read `presented` versus `discarded`. Jitter with healthy `frame` callbacks means you need presentation-time, not a bigger timer.

**Crisp text: viewporter plus fractional-scale-v1 plus xdg-output.** `wl_output::scale` is an integer, so on 150% it lies and text blurs. Three globals fix it together. `zxdg_output_manager_v1` (`unstable/xdg-output/xdg-output-unstable-v1.xml`, `zxdg_output_v1`; bind v2, cap at v3 - v3 only adds `name`/`description` events, so gate any `since=3` request behind `bound_version >= 3`) gives logical size and position. `wp_viewporter` (`stable/viewporter/viewporter.xml`, v1) sets destination size and source crop so the compositor scales. `wp_fractional_scale_manager_v1` (`staging/fractional-scale/fractional-scale-v1.xml`, v1) reports the true factor as `preferred_scale / 120`: 180 means 150%. Render at logical times scale, set the viewport, and integer-scale blur disappears. Viewporter without fractional-scale still guesses. Bind all three or stay blurry.

**single-pixel-buffer: solids without arrays.** Filling a 1080p SHM buffer with one color is waste. `wp_single_pixel_buffer_manager_v1` (`staging/single-pixel-buffer/single-pixel-buffer-v1.xml`, v1) offers `create_u32_rgba_buffer(r, g, b, a)`, which returns a one-pixel `wl_buffer` the compositor tiles. Use it for backgrounds, letterboxing, and drag highlights. One call, no `mmap`, no stride math.

**pointer-constraints plus relative-pointer: raw deltas.** Hiding the cursor is not locking it. `zwp_pointer_constraints_v1` (`unstable/pointer-constraints/pointer-constraints-unstable-v1.xml`, v1) offers `lock_pointer` for FPS cameras and `confine_pointer` for brushes, with `oneshot` or `persistent` lifetimes. Locked mode pairs with `zwp_relative_pointer_manager_v1` (`unstable/relative-pointer/relative-pointer-unstable-v1.xml`), which sends unaccelerated `relative_motion(dx, dy, dx_unaccel, dy_unaccel)`. Without it the camera spins at screen edges and accelerates with desktop settings. Request lock on click, read raw deltas while `locked`, destroy to release, always offer Esc to break out.

**idle-inhibit: stay awake only while it matters.** `zwp_idle_inhibit_manager_v1` (`unstable/idle-inhibit/idle-inhibit-unstable-v1.xml`, v1) offers `create_inhibitor(surface)`, which blocks blanking and suspend for that surface. Players inhibit while playing, presenters while presenting. Destroy the inhibitor on pause, minimize, or hide. A client that inhibits forever is a backpack that stays awake and cooks its battery. Compositors now expose who inhibits, and users blame correctly.

**layer-shell: chrome, not apps.** `zwlr_layer_shell_v1` (`wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml` in `wlr-protocols`, *not* `wayland-protocols`) offers `get_layer_surface` to create edge-anchored surfaces in background, bottom, top, and overlay layers with anchors and exclusive zones. Panels, docks, wallpapers, lockers, and keyboards live here. Regular apps keep out: an editor demanding top layer will be ignored. Building a bar starts here. Otherwise move on.

**xdg-decoration plus xdg-activation: frames and focus.** The decoration manager is `zxdg_decoration_manager_v1` (`unstable/xdg-decoration/xdg-decoration-unstable-v1.xml`, v1; see [Chapter 13, section 5](13-xdg-in-depth.md#5-decorations-who-draws-the-frame)). It negotiates client-side versus server-side frames per toplevel. Accept server-side when offered; fighting it yields double title bars. Activation is `xdg_activation_v1` (`staging/xdg-activation/xdg-activation-v1.xml`, v1): a launcher takes a token minted from a user action and passes it to `activate(token, surface)`. No token, no focus.

**primary-selection versus privileged snoopers.** The primary-selection manager is `zwp_primary_selection_device_manager_v1` (`unstable/primary-selection/primary-selection-unstable-v1.xml` in `wayland-protocols`). It mirrors `wl_data_device` exactly but for middle-click: select-to-copy, middle-click-to-paste, separate from Ctrl+C (see [Chapter 14](14-clipboard-dnd.md) for the core clipboard model). Show text and skip it, and middle-click users call you broken. Separately, `ext_data_control_manager_v1` (`ext_data_control_v1`, `staging/ext-data-control/ext-data-control-v1.xml` in `wayland-protocols`, privileged and allowlisted; distinct from wlroots `wlr-data-control`) and clipboard-manager protocols let a client read or inject every app's clipboard. Those are privileged and allowlisted. Never design a normal app around them.

**linux-dmabuf-v1 plus explicit-sync: GPU by reference.** SHM copies pixels; dmabuf passes them by reference. `zwp_linux_dmabuf_v1` (`stable/linux-dmabuf/linux-dmabuf-v1.xml`) builds params, `add`s each plane with fd, offset, stride, and modifier, then `create`s a `wl_buffer` the GPU already owns. Explicit-sync adds the fences that keep it tear-free: with unstable `zwp_linux_explicit_synchronization_v1` (`unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml`; track as `wp_linux_explicit_synchronization`) or its staging successor `wp_linux_drm_syncobj_v1` (`staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml`), call `set_acquire_fence` before commit and `get_release` after present. EGL and Vulkan paths must wire both; dmabuf without fences is the classic fast-but-torn bug.

**session-lock and the niche rest.** `ext_session_lock_manager_v1` (`ext-session-lock-v1`, `staging/ext-session-lock/ext-session-lock-v1.xml`, staging) lets a locker own the session securely. Input-method, color-management, and HDR extensions matter to their niches and nobody else. Bind them only when you are that tool.

## 15.5 Pick by Symptom

| Symptom | Bind this | Notes |
|---|---|---|
| Blurry on 125/150/175% | `wp_viewporter` (v1) + `wp_fractional_scale_manager_v1` (v1, staging) + `zxdg_output_manager_v1` (cap v2/v3) | Render at `preferred_scale/120`, set destination |
| Tearing, jittery scroll | `wp_presentation` (v2) + `zwp_linux_dmabuf_v1` + explicit-sync (`zwp_linux_explicit_synchronization_v1` unstable / `wp_linux_drm_syncobj_v1` staging) | Stamp commits, schedule on period, fence GPU buffers |
| Camera spins at edge | `zwp_pointer_constraints_v1` (v1, unstable) + `zwp_relative_pointer_manager_v1` | Lock, read raw `dx/dy_unaccel`, never warp |
| Never sleeps | Audit `zwp_idle_inhibit_manager_v1` (v1, unstable) | Create while playing only, destroy on pause |
| Panel buried | `zwlr_layer_shell_v1` (`wlr-protocols`, not `wayland-protocols`) | Top/overlay layer, anchor plus exclusive zone; apps keep out |
| Double title bars | `zxdg_decoration_manager_v1` (v1, unstable) | Accept server-side when offered; see ch 13 s5 |
| Launcher opens behind | `xdg_activation_v1` (v1, staging) | Token from click into `activate`; no token, no focus |
| Middle-click dead | `zwp_primary_selection_device_manager_v1` (unstable) | Mirror clipboard offers in separate store; see ch 14 |
| Solid fill costs MBs | `wp_single_pixel_buffer_manager_v1` (v1, staging) | One RGBA pixel, compositor tiles it |
| Locker bypassable | `ext_session_lock_manager_v1` (staging), color/IME presets | Niche; bind only for that role |

## Recap

Core is grammar; extensions are vocabulary. Stable means rely on it (`wp_presentation` v2, `wp_viewporter` v1, `zwp_linux_dmabuf_v1`), staging means cap the version (`wp_fractional_scale_manager_v1` v1, `wp_single_pixel_buffer_manager_v1` v1, `xdg_activation_v1` v1, fifo-v1, ext-session-lock), unstable means gate on the exact name and keep a fallback (`zwp_pointer_constraints_v1` v1, `zwp_idle_inhibit_manager_v1` v1, `zxdg_decoration_manager_v1` v1, `zwp_primary_selection_device_manager_v1`, explicit-sync). Binding never changes: `min(yours, theirs)`, store the version, guard new requests, degrade gracefully. The extensions in 15.4 cover nearly all visible pain: timing, scaling, buffers, pointer, power, chrome, frames, focus, selection, and zero-copy present.

**Exercises:**

1. **List the globals.** Write a registry lister that prints every `interface`, `name`, and `version`, then marks which of this chapter's extensions are present and at what version. Run it on two compositors and note each missing global's tier. For every missing unstable global, write one sentence describing your fallback.
2. **Check the version.** Extend the bind snippet to add `wp_fractional_scale_manager_v1` (v1, staging) and `xdg_activation_v1` (v1, staging). Cap each at your compile-time version, store the bound version, and gate any `since=2+` request behind `bound_version >= 2` (neither interface has a v2 yet, so the guard must hold vacuously today). Run against a version-1 compositor, confirm the guard holds, and verify with `WAYLAND_DEBUG=1` that no newer request is sent.
