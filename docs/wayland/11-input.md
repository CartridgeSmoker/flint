# Chapter 11: Input - Seats, Pointer, Keyboard, and Touch

> **Objectives**
> - Explain what a `wl_seat` represents and how to hot-plug its capabilities.
> - Use input serials as short-lived permissions for privileged requests.
> - Accumulate `wl_pointer` and `wl_touch` bursts and dispatch them on `frame`.
> - Implement cursors, motion, buttons, and axis scrolling.
> - Set up XKB and translate evdev scancodes to keysyms and UTF-8.
> - Track touch points by ID across `down`, `motion`, `up`, and `cancel`.

Every surface so far has been deaf: visible but untouchable. Input makes it alive. Wayland splits input into three device interfaces - `wl_pointer`, `wl_keyboard`, `wl_touch` - all owned by a parent called `wl_seat`. They share two ideas that cause most beginner bugs: serials and frames.

## 11.1 The seat: one person, one set of devices

A seat models *one user at one set of hardware*: a keyboard plus a mouse plus a touchscreen used by one person is one seat. A second plugged-in keyboard joins the same seat rather than creating a new one; the layout switches dynamically and the compositor decides the policy, so seats may not map 1:1 to physical reality.

Almost every machine has exactly one seat, conventionally `seat0`. Never hard-code that string; use the `name` event if you display it. True multi-seat - two users, two monitor/keyboard sets, one machine - exists in kiosks and labs but is rare. Most clients bind the first seat and ignore the rest.

Bind at version 7, the current maximum (`name` v2+, `release` seat v5+):

```c
if (strcmp(interface, wl_seat_interface.name) == 0)
    state->seat = wl_registry_bind(reg, name, &wl_seat_interface, 7);
```

A seat starts empty and announces what it can do through a `capabilities` bitfield: `1` = pointer, `2` = keyboard, `4` = touch, ORed together. A laptop with all three reports `7`; a touchscreen kiosk reports `4`. The `capabilities` event fires whenever devices appear or disappear, and `name` carries the identifier.

The pattern is create-on-appear, release-on-disappear. When the pointer bit appears, call `wl_seat_get_pointer` and attach listeners; when it vanishes, call `wl_pointer_release`. Same for keyboard and touch. Note the quirk: the destructors are named `release`, not `destroy`, but they free the object the same way. The seat itself is released with `wl_seat_release` (seat v5+) at teardown. The seat survives device replugs, so a replugged mouse simply triggers another `get_pointer`:

```c
static void
seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    struct client_state *state = data;

    bool have_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
    bool have_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    bool have_touch = caps & WL_SEAT_CAPABILITY_TOUCH;

    if (have_pointer && !state->pointer) {
        state->pointer = wl_seat_get_pointer(state->seat);
        wl_pointer_add_listener(state->pointer, &pointer_listener, state);
    } else if (!have_pointer && state->pointer) {
        wl_pointer_release(state->pointer);
        state->pointer = NULL;
    }

    if (have_keyboard && !state->keyboard) {
        state->keyboard = wl_seat_get_keyboard(state->seat);
        wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
    } else if (!have_keyboard && state->keyboard) {
        wl_keyboard_release(state->keyboard);
        state->keyboard = NULL;
    }

    if (have_touch && !state->touch) {
        state->touch = wl_seat_get_touch(state->seat);
        wl_touch_add_listener(state->touch, &touch_listener, state);
    } else if (!have_touch && state->touch) {
        wl_touch_release(state->touch);
        state->touch = NULL;
    }
}
```

### Where devices live

```
wl_registry
    |
    +-- wl_seat ("seat0", caps = pointer | keyboard | touch)
            |
            +-- wl_pointer  -- enter / leave / motion / button / axis / source / stop / discrete / frame
            |
            +-- wl_keyboard -- keymap / enter / leave / key / modifiers / repeat_info
            |
            +-- wl_touch    -- down / up / motion / cancel / shape / orientation / frame
```

The registry finds seats, seats hand out devices, devices emit events. You never construct a pointer directly; you always ask the seat.

## 11.2 Serials are permission slips

Serials are carried by pointer `enter`/`leave`/`button`, keyboard `enter`/`leave`/`key`/`modifiers`, and touch `down`/`up`. Motion, `axis` (and its `source`/`stop`/`discrete` companions), and touch `motion` carry only a timestamp, never a serial. Each serial is a monotonically increasing number minted by the compositor. Treat it as a permission slip proving "this input really happened just now, on this seat."

Privileged requests require a recent serial, and the compositor silently ignores stale or invented ones. This stops a background window from popping up menus or stealing focus unprompted. A serial is rejected when it is too old, was minted for a different surface, or is the wrong kind for the request - for example, a `wl_shell_surface` popup grab keyed to a pointer wiggle (motion carries no serial at all) is rejected, while the same grab keyed to a button-press serial on the click that opened it is allowed. The rules:

1. Save the serial from the event that justifies your next request.
2. Spend it promptly, on the same seat.
3. Never invent one.

A pointer button-press serial authorizes popup menus and interactive drags (`xdg_surface.move` / `resize`); a pointer-enter serial authorizes the initial `set_cursor`. A key-press, keyboard-enter, or modifiers serial authorizes keyboard-driven menus, clipboard copies, and selection popups. A common bug is caching one serial at startup and reusing it forever. Keep `last_pointer_serial` and `last_keyboard_serial` fields, update the pointer one on every pointer enter/leave/button and the keyboard one on every keyboard enter/leave/key (the `keyboard_handle_key` snippet below stores only the press serial, so store the enter serial in your enter handler too), and pass the fresh value each time.

## 11.3 Frames: input arrives in bursts

Pointer and touch events arrive in bursts terminated by `frame`. The compositor may send `enter`, `motion`, `button`, and `axis` (plus `source`/`stop`/`discrete`) before a single `frame`. Accumulate the burst into a struct plus a "what changed" bitmask, run application logic exactly once on `frame`, then clear the accumulator. Touch `shape` and `orientation` join the same burst and are consumed on `touch.frame`.

Keyboard events are self-contained - each `key`, `modifiers`, and `enter`/`leave` stands alone with no frame.

Batching exists because one physical gesture produces several protocol events in the same timestamp group. Dispatching once per frame avoids redrawing three times for one flick and keeps button-plus-motion state consistent.

## 11.4 Pointer: position, buttons, scroll, cursor

Positions arrive as `wl_fixed_t`; convert with `wl_fixed_to_double`. They are surface-local, with `(0, 0)` at the top-left of the surface under the cursor.

**Enter and leave** bracket hover. `enter` gives a serial, the surface entered, and `x`/`y`. Save the serial for `set_cursor`. `leave` gives a serial and the surface left. Allocate hover state on enter, free it on leave.

**Cursor images.** A surface starts with no cursor. To set one you need the latest enter serial, a `wl_surface` for the image, and a hotspot: the pixel inside the image that marks the true tip. Then:

```c
wl_pointer_set_cursor(pointer, enter_serial, cursor_surface, hotspot_x, hotspot_y);
wl_surface_attach(cursor_surface, cursor_buffer, 0, 0);
wl_surface_commit(cursor_surface);
```

Passing `NULL` hides the cursor, correct for games. Most clients load a theme via `wayland-cursor`: `wl_cursor_theme_load("default", 24, shm)`, pick `"left_ptr"`, attach its buffer, and re-commit on every change, reusing a fresh enter serial after re-entry.

**Motion** carries a millisecond timestamp and new `x`/`y`, and fires only over your own surfaces. Motion elsewhere goes to whoever owns that surface; you hear nothing.

**Buttons** carry a serial, timestamp, Linux input code (`linux/input-event-codes.h`), and pressed/released state. The common codes are `BTN_LEFT` (`0x110`, 272), `BTN_RIGHT` (`0x111`), and `BTN_MIDDLE` (`0x112`). Save the press serial - it authorizes drags and menus - and track held state yourself, since the protocol offers no query.

**Axis (scroll)** reports the scrolled amount. The `axis` field selects vertical (`0`) or horizontal (`1`); `value` is the amount. The `source` field (pointer v5+) names the origin: `wheel` for notched wheels, `finger` for two-finger trackpad swipes, `continuous` for smooth high-resolution wheels, `tilt` for sideways wheel tilt. Two companions complete the picture: `axis_stop` (pointer v5+) ends a kinetic sequence when fingers lift, and `axis_discrete` (pointer v5+) reports classic wheel notches for clients that count clicks. Accumulate axis values and act on `frame`:

```c
struct pointer_accum {
    double x, y, axis_v, axis_h;
    uint32_t changed; /* MOTION | BUTTON | AXIS bits */
};

static void
pointer_handle_axis(void *data, struct wl_pointer *pointer,
        uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct client_state *state = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        state->ptr.axis_v += wl_fixed_to_double(value);
    else
        state->ptr.axis_h += wl_fixed_to_double(value);
    state->ptr.changed |= PTR_AXIS;
}

static void
pointer_handle_frame(void *data, struct wl_pointer *pointer)
{
    struct client_state *state = data;
    if (state->ptr.changed & PTR_AXIS)
        scroll_view(state, state->ptr.axis_h, state->ptr.axis_v);
    if (state->ptr.changed & PTR_MOTION)
        update_hover(state, state->ptr.x, state->ptr.y);
    state->ptr.changed = 0;
    state->ptr.axis_v = state->ptr.axis_h = 0;
}
```

## 11.5 Keyboard and the XKB primer

Keyboards confuse newcomers because a physical key is not a letter. Three layers must stay separate:

- **Scancode (keycode):** *where* the finger landed - a number for the physical key. The Q position is the same key on French and American boards.
- **Keysym:** *what the key means now*, after layout, modifiers, and groups. That position yields `q` on US layout, `a` on French, `Q` with Shift.
- **Unicode text:** characters to insert after dead-key and input-method composition. Keysyms feed shortcuts; full text input via `zwp_text_input` is a separate protocol.

Above these sit **modifiers** (Shift, Ctrl, Alt, Super), **latches** (a one-shot modifier applying to exactly the next key), and **groups** (whole-layout switches such as English versus Greek on Alt+Shift).

**Keymap setup.** The compositor sends the layout as a `keymap` event: fd, byte size, format. The format enum defines `NO_KEYMAP` (`0`, never actually sent) and `XKB_V1` (`1`); in practice only `WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1` arrives. `mmap` the fd, compile with libxkbcommon, then `munmap` and `close`. The compositor may replace the keymap at runtime, so rebuild state on every event. Create the context once with `xkb_context_new(XKB_CONTEXT_NO_FLAGS)`:

```c
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
        uint32_t format, int fd, uint32_t size)
{
    struct client_state *state = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    /* MAP_PRIVATE here, MAP_SHARED in ch 12: both work for a read-only mapping. */
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        state->xkb_context, map,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (!keymap)
        return;
    struct xkb_state *xkb_state = xkb_state_new(keymap);
    xkb_keymap_unref(state->xkb_keymap);
    xkb_state_unref(state->xkb_state);
    state->xkb_keymap = keymap;
    state->xkb_state = xkb_state;
}
```

**Translating keys.** `wl_keyboard.key` delivers a serial, timestamp, evdev scancode, and state: `released` (`0`) or `pressed` (`1`). That is the whole wire enum - the compositor never sends repeat over the wire; key repeat is synthesized client-side from `repeat_info` (see below). Add 8 before calling XKB - evdev starts at 0, XKB numbering at 8 - then query both meaning and text:

```c
static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, uint32_t time, uint32_t key, uint32_t key_state)
{
    struct client_state *state = data;
    if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED)
        state->last_keyboard_serial = serial;
    if (!state->xkb_state)
        return;
    xkb_keycode_t code = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(state->xkb_state, code);
    char buf[64];
    int n = xkb_state_key_get_utf8(state->xkb_state, code, buf, sizeof(buf));
    /* key_state is only pressed/released; collapse to bool so no "repeated" branch can creep in. */
    handle_keypress(state, sym, buf, (size_t)n,
        key_state == WL_KEYBOARD_KEY_STATE_PRESSED);
}
```

> **Misconception callout: "scancode = letter"? No.**
>
> Writing `char c = key + 'a'` or switching on the raw scancode produces "my app types QWERTY on AZERTY keyboards." The scancode is a *position*; the letter lives on the far side of XKB after adding 8 and applying layout, modifiers, and group. Scancode `24` is `q` in American, `a` in French, `Q` with Shift. Always route through `xkb_state_key_get_one_sym` for shortcuts and `xkb_state_key_get_utf8` for text.

**Modifiers, focus, repeat.** Forward `modifiers` (which carries its own serial - save it like a key-press serial) via `xkb_state_update_mask` *before* translating the next key or Shift lags one stroke behind. `enter` gives a serial, the focused surface, and `keys` - a `wl_array` of currently-held `u32` evdev scancodes - save the enter serial and seed held-key tracking from that array. `leave` gives a serial and the old surface; clear all held keys or a key released elsewhere looks stuck forever. `repeat_info` (keyboard v4+) gives rate and delay for client-side repeat: re-dispatch every `1000 / rate` ms after `delay` until release. Rate 0 disables repeat; the compositor never repeats for you. Free the keyboard object with `wl_keyboard_release` (keyboard v3+).

## 11.6 Touch: fingers on lease

Touch goes to the surface under the finger, like pointer motion, but with no cursor and no hover. Each finger is tracked by an **ID** assigned in `touch.down` and valid until its `touch.up`. IDs are leases, not identities: once a finger lifts, its ID may be reused for a different finger. Use it only as a dictionary key for one press.

- **`down`:** serial, timestamp, surface, ID, `x`/`y` (surface-local fixed-point). Allocate a touch-point keyed by ID.
- **`motion`:** timestamp, ID, new `x`/`y`.
- **`up`:** serial, timestamp, ID. Free the touch-point.
- **`cancel`:** aborts the *entire* sequence - the compositor reclaimed the gesture, e.g. a system swipe. Drop all active points at once; no `up` events follow. Skipping this leaves phantom fingers on screen.
- **`frame`:** ends a burst, exactly like pointer frame. A two-finger pinch yields two motions plus one frame; run pinch logic here.
- **`shape` / `orientation` (touch v6+):** major/minor axes of the contact ellipse and its rotation, in surface units. `orientation` is the angle in degrees between the ellipse major axis and the Y-axis. Brushes and palm rejection use these; plain buttons ignore them.

All touch timestamps are monotonic milliseconds with an arbitrary epoch, not wall-clock time - suitable only for comparing against each other.

Unlike keyboards there is no translation layer, and unlike pointers there are no buttons or cursors - contact is the button.

## Recap

- A `wl_seat` is one user's input. Bind it (v7 max; `name` v2+, `release` seat v5+), watch `capabilities` (`1` pointer, `2` keyboard, `4` touch) plus `name`, and create each device on appearance with `get_*`, freeing with `release` (keyboard v3+) on disappearance. A second keyboard joins the same seat; the compositor decides layout policy.
- Serials are short-lived permissions carried by pointer enter/leave/button, keyboard enter/leave/key/modifiers, and touch down/up (motion/axis/touch-motion carry only time). Save the press/enter serial and spend it promptly on the same seat; stale, wrong-surface, or wrong-kind serials are ignored.
- Pointer and touch batch to `frame`: accumulate, dispatch once, clear. Keyboard events stand alone.
- Pointers give enter/leave (save the serial), motion, buttons (`BTN_LEFT 0x110` et al.), axis with source/stop/discrete (all pointer v5+), and `set_cursor` with enter serial, hotspot, and `NULL`-to-hide.
- Keyboards speak evdev scancodes: `mmap` the `keymap` fd (`NO_KEYMAP` 0 never sent, only `XKB_V1`), add 8, translate with `get_one_sym` / `get_utf8`, track `modifiers` (carries serial) via `update_mask`, focus via enter (serial plus `wl_array` of u32 evdev codes) / leave, and repeat client-side from `repeat_info` (keyboard v4+; wire key state is only pressed/released, never repeat).
- Touches are ID leases: `down` allocates, `motion` updates, `up` frees, `cancel` frees everything, `frame` groups gestures, `shape`/`orientation` (touch v6+; orientation is degrees between major axis and Y-axis) serve brushes. Timestamps are monotonic ms, not wall-clock.

## Exercises

1. **Log enter and leave.** Bind the first seat, create pointer and keyboard on capability appearance, and print every `pointer.enter` / `pointer.leave` and `keyboard.enter` / `keyboard.leave` with serial, surface, and converted `x`/`y` for pointer events. Cross two of your surfaces and switch keyboard focus. Confirm serials increase monotonically and every enter pairs with a leave.
2. **Print keysym plus UTF-8.** Compile the `keymap` payload, update the mask on `modifiers`, and on each `key` print the evdev scancode, the `+8` keycode, the keysym name via `xkb_keysym_get_name`, and UTF-8 bytes via `xkb_state_key_get_utf8`. Test with Shift+letter, a second layout if available, and a dead-key accent. Verify the scancode stays fixed while keysym and text change.
