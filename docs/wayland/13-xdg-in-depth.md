# Chapter 13: XDG Shell in Depth

> **Objectives.** By the end of this chapter you will be able to:
> - Explain the two-part configure event and why its order is load-bearing.
> - Implement correct acknowledge logic, including zero-size handling and state flags.
> - Request maximize, fullscreen, minimize, and size limits without drawing optimistically.
> - Create, nest, grab, and tear down popups without protocol errors.
> - Use `move`, `resize`, and `show_window_menu` instead of self-positioning.
> - Negotiate client versus server decorations.
> - Fill in a one-shot `xdg_positioner` that expresses intent instead of demanding coordinates.

Chapter 12 got a window on screen. This chapter makes it a good desktop citizen. `xdg-shell` is a negotiation: the client asks, the compositor decides, and both sides synchronize through configure serials. Get this wrong and windows freeze, flicker, or die with protocol errors. Get it right and tiling, HiDPI, popups, and decorations work for free.

## 1. Configure and Acknowledge, for Real

All toplevel state flows through one pattern: the compositor sends configuration, the client acknowledges, then commits a matching buffer.

Configuration arrives as *two* events in fixed order:

1. `xdg_toplevel.configure(width, height, states)` — what the compositor wants.
2. `xdg_surface.configure(serial)` — which batch this belongs to.

The order is load-bearing. Buffer the toplevel values until you see the surface serial, then acknowledge with `xdg_surface.ack_configure(serial)` and commit a buffer that matches. The serial is the only thing tying a size to a frame.

### What zero means

`width` and `height` describe window geometry *excluding* shadows and invisible padding. Nonzero means resize to exactly this. Zero means "I defer to you": keep your current size, or pick your natural size on first map. Never create a zero-size buffer. Floating windows typically get `0,0` on first map; tiled or maximized windows get a concrete size.

### `set_window_geometry`: the mechanism behind "excludes shadows"

The configure size describes the *window geometry*, not the full surface. Call `xdg_surface.set_window_geometry(x, y, width, height)` to declare which part of your surface is the actual window: shadows, glow, and invisible padding live outside it, and input, tiling, and popup anchoring use the geometry, not the buffer extents. Coordinates are surface-local (relative to your committed buffer). The setting is double-buffered: it takes effect on the next `wl_surface.commit`, like buffer attachment. It cannot be unset, only replaced with a new rectangle; pass a zero width or height and the compositor raises `invalid_size`. Set it before the first commit and update it whenever shadows or borders change, otherwise tiled windows keep phantom margins and popups anchor to the glow instead of the button.

### State flags are facts

The `states` array reports truth: `maximized`, `fullscreen`, `resizing`, `activated`, `tiled_left`, `tiled_right`, `tiled_top`, `tiled_bottom`, and `suspended`, plus `constrained_left`, `constrained_right`, `constrained_top`, `constrained_bottom` (values 10-13, since v6, set when a tiled edge presses against a screen edge or strut). Draw focus highlights only when `activated` is present. While `resizing` is set, expect a stream of configures and keep redraws cheap. When `suspended` appears (occluded, minimized, or on another workspace), stop rendering until it clears. When tiled flags appear, trim shadows and rounded corners that would bleed onto a neighbor.

### Acknowledge discipline

Three rules keep you alive: never ack a serial twice (once consumed it is gone), never ack an older serial after a newer one, and never ack a serial the compositor never sent (that is the `invalid_serial` protocol error). Note that acking every pending configure in order - say serials 42, 43, 44 each with `ack_configure` - and then committing once is LEGAL; the "ack latest only" shorthand is an optimization to skip intermediate sizes during resize, not a protocol requirement. What actually kills is re-acking a consumed serial, acking older-after-newer, acking a never-sent serial, or committing new content under a stale size (flicker, not a protocol error, but visible).

The naive client stores sizes in globals and acks on receipt. The polite client queues pending state and acks at commit time:

```c
/* NAIVE: races under back-to-back configures. */
static void
naive_toplevel_configure(void *data, struct xdg_toplevel *t,
                         int32_t w, int32_t h, struct wl_array *s)
{
    struct window *win = data;
    win->width = w; win->height = h; /* overwritten before commit */
}
static void
naive_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    xdg_surface_ack_configure(s, serial); /* acked before buffer ready */
}

/* SERIAL-AWARE: batch until commit. */
static void
good_toplevel_configure(void *data, struct xdg_toplevel *t,
                        int32_t w, int32_t h, struct wl_array *states)
{
    struct window *win = data;
    win->pending.width = (w == 0) ? win->current.width : w;
    win->pending.height = (h == 0) ? win->current.height : h;
    win->pending.maximized = has_state(states, XDG_TOPLEVEL_STATE_MAXIMIZED);
    win->pending.activated = has_state(states, XDG_TOPLEVEL_STATE_ACTIVATED);
    win->pending_valid = true;
}

static void
good_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    struct window *win = data;
    win->pending_serial = serial;
    win->pending_valid = true;
    /* Do NOT ack here. Schedule a frame callback; redraw() below
       acks immediately before commit so one batch gets one ack
       and one matching frame. */
    schedule_redraw(win);
}
/* In redraw(): render at pending size, ack_configure(pending_serial),
   attach buffer, commit, clear pending_valid. */
```

`redraw()` renders at the pending size, calls `ack_configure(pending_serial)` immediately before attach/commit, and commits. One batch, one ack, one matching frame. Storing the serial in the configure handler and acking inside redraw (driven by a frame callback) is what "ack at commit" means; calling `redraw()` synchronously inside the configure handler would ack before the compositor is ready for the frame.

### Close is a request

`xdg_toplevel.close` means the user asked to close the window, via Alt+F4 or a titlebar button. The compositor will not destroy your surfaces. Save quickly and quit cleanly. Convention (toolkit policy, NOT protocol - the spec permits ignoring `close`): many toolkits treat a second `close` while a save dialog is open as unconditional and quit without waiting. That is a UI choice, not a protocol obligation.

## 2. Client to Compositor: How to Ask

Clients never set state directly. They request, then wait for a configure that confirms:

- `set_min_size(w, h)` / `set_max_size(w, h)` — call early, before first commit, so the compositor never suggests an impossible size. Zero clears a limit. Minimum larger than maximum is a protocol error.
- `set_maximized()` / `unset_maximized()` — ask to fill the output minus panels. Keep your floating size for restore.
- `set_fullscreen(output_or_null)` / `unset_fullscreen()` — pass an `wl_output` to target a monitor, or NULL to let the compositor choose.
- `set_minimized()` — ask to be iconified. There is no unset; only the user or taskbar restores you.

The sequence is always request, configure, ack, commit:

```
 client                    compositor
   |  set_maximized()          |
   |-------------------------->|
   |  toplevel.configure(1920x1030, [maximized, activated])
   |  surface.configure(serial=7)|
   |<--------------------------|
   |  ack_configure(7) + commit maximized buffer
   |-------------------------->|
```

> **Misconception callout.** "`set_maximized()` draws maximized now, right?" No. Nothing changes until the compositor replies with a configure carrying `maximized` and usually a new size. Drawing a fullscreen-sized buffer immediately causes flicker and clipped animations. Request, wait, then honor what you got, not what you asked.

## 3. Popups: Menus and Nested Chains

Every popup — menu, combo box, tooltip, submenu — is its own `wl_surface` plus `xdg_surface` plus `xdg_popup`, created with `get_popup(parent, positioner)`. The parent ties stacking and lifetime: popups render above their parent and die with it.

Nesting is explicit. A `File -> Recent -> Document.odt` chain is three popups, each parented to the previous one. Dismissal walks back down the chain.

```c
/* Fragment: assumes compositor, wm_base, parent_xs, positioner,
   popup_listener, popup_surface_listener, and menu are declared.
   Initial commit maps the popup; grab (below) must precede it. */
struct wl_surface *s = wl_compositor_create_surface(compositor);
struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, s);
struct xdg_popup *popup = xdg_surface_get_popup(xs, parent_xs, positioner);
xdg_positioner_destroy(positioner); /* one-shot, see section 6 */

xdg_popup_add_listener(popup, &popup_listener, menu);
xdg_surface_add_listener(xs, &popup_surface_listener, menu);
/* Commit to map. If grabbing, call xdg_popup_grab() BEFORE this
   commit; grabbing after the popup is mapped raises the
   invalid_grab protocol error. */
wl_surface_commit(s);
```

Key behaviors:

- **`xdg_popup.configure(x, y, width, height)`** gives geometry *relative to the parent*. Honor what you got, not what your positioner asked. The compositor may slide or flip you on screen. Size your buffer to the configured size and ack the accompanying surface serial.
- **`grab(seat, cause_serial)`** routes keyboard input so arrows and Escape work, and lets outside clicks dismiss. Pass the serial of the click or key that opened the menu. The grab MUST be requested before the popup is mapped (before the initial commit); calling `grab` after mapping raises the `invalid_grab` protocol error. If the compositor denies the grab (stale serial, another client grabbed first), it immediately sends `popup_done` dismissing the popup: tear down on `popup_done` and do NOT keep an ungrabbed menu open. There is no "ungrabbed-open" state.
- **`popup_done`** means dismiss now. Destroy popups you own, clear your pointers, and ignore late duplicates that arrive after teardown. Destroy strictly top-down, child before parent: destroying a non-topmost popup raises the `xdg_wm_base.not_the_topmost_popup` protocol error (since v2). Tear down eagerly on Escape or outside click.

## 4. Why You Cannot Position Yourself

Wayland removed client-side absolute positioning: panels, tiling, and rotation make client-chosen coordinates wrong as often as right. Interactive placement goes through three `xdg_toplevel` verbs, each taking the `wl_seat` and the input serial authorizing the gesture:

- `move(seat, serial)` — begin a compositor-driven drag, typically from a headerbar press.
- `resize(seat, serial, edges)` — begin a resize from a border hit-test. `edges` is a bitmask: `top`, `bottom`, `left`, `right`, and corners such as `top_left`. Expect `resizing`-flagged configures while dragging.
- `show_window_menu(seat, serial, x, y)` — ask for the compositor menu (minimize, maximize, move, close) at surface-local `x,y`, typically on titlebar right-click.

If you draw your own decorations, these three are mandatory. Compositors ignore button drags that bypass `move` and `resize` because they cannot distinguish them from clicks.

## 5. Decorations: Who Draws the Frame?

`xdg-shell` defaults to client-side decorations: you draw your own titlebar. To ask the compositor to draw it, use `zxdg_decoration_manager_v1`:

```xml
<interface name="zxdg_decoration_manager_v1" version="1">
  <request name="destroy" type="destructor"/>
  <request name="get_toplevel_decoration">
    <arg name="id" type="new_id" interface="zxdg_toplevel_decoration_v1"/>
    <arg name="toplevel" type="object" interface="xdg_toplevel"/>
  </request>
</interface>
<interface name="zxdg_toplevel_decoration_v1" version="1">
  <request name="destroy" type="destructor"/>
  <request name="set_mode"><arg name="mode" type="uint" enum="mode"/></request>
  <request name="unset_mode"/>
  <event name="configure"><arg name="mode" type="uint" enum="mode"/></event>
  <enum name="mode">
    <entry name="client_side" value="1"/>
    <entry name="server_side" value="2"/>
  </enum>
</interface>
```

Create one decoration object per toplevel, call `set_mode(server_side)` (value 2) or `set_mode(client_side)` (value 1), then wait. The `configure` event carrying a `mode` is the decision. `unset_mode()` is the humble default — no preference — and the right choice unless the user picked a style. Destroy each decoration object with `destroy` when its toplevel goes away, and `destroy` the manager at shutdown. Warning: hardcoding client-side mode while your toolkit also draws a headerbar produces double titlebars with two close buttons.

## 6. Positioners Are One-Shot Intent

`xdg_positioner` describes where a popup *wants* to be; the compositor decides where it goes. The object is single-use: fill it, pass it to `get_popup`, then destroy it. Never reuse it.

`set_size(w, h)` is required; zero or negative size raises the `xdg_positioner.invalid_input` error (and a malformed positioner passed to `get_popup` raises `xdg_wm_base.invalid_positioner`). `set_anchor_rect(x, y, w, h)` selects the rectangle in parent coordinates you attach to, such as the spawning button. `set_anchor(edge)` and `set_gravity(direction)` each take one of `none`, `top`, `bottom`, `left`, `right`, `top_left`, `bottom_left`, `top_right`, `bottom_right`, picking which side of that rect meets which side of the popup. `set_constraint_adjustment(flags)` authorizes fixes when the wanted position overflows, using the `xdg_positioner.constraint_adjustment` enum: `none` (0), `slide_x` (1), `slide_y` (2), `flip_x` (4), `flip_y` (8), `resize_x` (16), `resize_y` (32). There are no combined `slide`/`flip`/`resize` enumerants: each axis is independent, so sliding horizontally says nothing about sliding vertically. `set_offset`, `set_reactive`, `set_parent_size`, and `set_parent_configure` (the last three since v3) fine-tune.

Think intent, not coordinates. Tooltips want `slide_x | slide_y` so they nudge on screen; combo boxes want `flip_y` so they open upward when there is no room below. A sane menu default is `slide_x | slide_y | flip_x | flip_y | resize_x | resize_y` (or a subset you have tested). Requesting `none` and demanding exact coordinates is how menus end up clipped or half offscreen.

A NULL parent passed to `get_popup` is legal: it covers the case where the parent surface comes from another protocol and cannot be designated yet, as long as the parent relationship is set before the initial commit.

## 7. Version Notes: What v3-v7 Added

Readers generating from the v7 XML (`/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml`, version 7) get more than the v1 core above:

- **v3:** popup `reposition` request plus `repositioned` event and `reposition_token`, so a live menu can be moved without destroying it; positioner `set_reactive`, `set_parent_size`, and `set_parent_configure` for menus that track a scrolling or resizing parent.
- **v4:** toplevel `configure_bounds(width, height)`, the maximum size the compositor recommends, sent before the first configure. Ignoring it mis-sizes initial windows on constrained outputs (panels, tiling).
- **v5:** toplevel `wm_capabilities` event with the `wm_capabilities` enum (`window_menu`, `maximize`, `fullscreen`, `minimize`): which verbs the compositor actually honors. Ignoring it offers dead menu items (a Maximize button that can never succeed).
- **v6:** the `constrained_*` states listed in section 1.

Bind version 7 when available but code defensively: check the bound version before using `configure_bounds` or `wm_capabilities`, and fall back to the v1 behavior when talking to an older compositor.

## Recap

Configure flows toplevel-first then surface-serial; zero means choose; ack latest once and commit matching pixels. Maximize, fullscreen, minimize, and size limits are wishes until a configure confirms them. Popups are per-surface triples with parent chains, honored geometry, seat grabs, and top-down teardown on `popup_done`. Placement uses `move`, `resize`, and `show_window_menu`. Decorations are negotiated, with `unset_mode` as the polite default. Positioners are one-shot intent where `set_size` plus `slide_x | slide_y | flip_x | flip_y` (X/Y independent, plus `resize_*`) keeps popups visible. Ground truth is `/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml`, version 7: when this chapter and the XML disagree, the XML wins.

### Exercises

1. **Tiling shadow trim.** When any tiled flag is present, drop shadows and square the touching corners; restore them when the flags clear. Verify by tiling left and right and confirming no shadow bleeds onto the neighbor while floating windows keep theirs.
2. **Nested popup teardown.** Build a two-level menu plus submenu. Dismiss via Escape, outside click (`popup_done`), and main-window close. Confirm each path destroys child-before-parent with no protocol error, clears dangling pointers, and tolerates a duplicate late `popup_done`.
