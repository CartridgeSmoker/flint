# Chapter 12: Logging Seat Input - A Complete Example Client

> **Objectives**
> - Grow the running example client to log all seat input: pointer, keyboard, and touch.
> - Store `wl_seat`, `wl_pointer`, `wl_keyboard`, and `wl_touch` in `client_state` with hotplug-safe create and release.
> - Accumulate pointer bursts behind an `event_mask` and print them once per `frame`.
> - Compile XKB keymaps, translate scancodes to keysyms and UTF-8, and track modifiers.
> - Track up to ten touch points by ID and flush them on `frame` or `cancel`.
> - Build with `wayland-client` plus `xkbcommon` and read the log output.

The client you have built so far can map a window and survive resizes, but it is still deaf. This chapter makes it listen. Starting from the surface and xdg-shell code of the previous chapters, you add a seat and its three devices and log everything they say. The result is roughly two and a half times as much code, and almost all of the growth is input handling. That ratio is worth noticing: in a real Wayland client, input handling dwarfs window setup.

The approach is deliberately boring: no toolkit, no cursor theme, no actions. Every event is accumulated and then pretty-printed to stdout so you can see the protocol rhythm. Once you can read this log fluently, adding real behavior - dragging, typing, pinching - is a small step.

## 12.1 State, registry, and the seat listener

Everything hangs off one state struct. Extend it with four Wayland objects and three XKB objects:

```c
struct client_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;

    /* new in this chapter */
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_touch *touch;

    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    uint32_t last_keyboard_serial;

    struct pointer_accum pointer_accum;
    struct touch_accum touch_accum;
};
```

The pointer and touch accumulators are defined in the next sections. The XKB fields start as `NULL` and are filled in later.

Registry handling gains one branch. Bind the seat at version 7, which is the highest version any released compositor supports for `wl_seat`, and attach the seat listener immediately:

```c
static void
registry_handle_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version)
{
    struct client_state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(
            registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->xdg_wm_base = wl_registry_bind(
            registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = wl_registry_bind(
            registry, name, &wl_seat_interface, 7);
        wl_seat_add_listener(state->seat, &seat_listener, state);
    }
}
```

Version 7 matters because version 2 added the `name` event and version 5 added `release`. Binding lower would silently drop them. Binding higher than the compositor offers is safe - libwayland clamps down - but requesting 7 documents intent: you want everything up to and including `release`.

The seat listener has two events, and the first version of this client left one of them as a `TODO`. Here is the complete version:

```c
static void
seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    struct client_state *state = data;

    bool have_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
    bool have_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    bool have_touch = caps & WL_SEAT_CAPABILITY_TOUCH;

    if (have_pointer && state->pointer == NULL) {
        state->pointer = wl_seat_get_pointer(state->seat);
        wl_pointer_add_listener(state->pointer, &pointer_listener, state);
    } else if (!have_pointer && state->pointer != NULL) {
        wl_pointer_release(state->pointer);
        state->pointer = NULL;
    }

    if (have_keyboard && state->keyboard == NULL) {
        state->keyboard = wl_seat_get_keyboard(state->seat);
        wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
    } else if (!have_keyboard && state->keyboard != NULL) {
        wl_keyboard_release(state->keyboard);
        state->keyboard = NULL;
    }

    if (have_touch && state->touch == NULL) {
        state->touch = wl_seat_get_touch(state->seat);
        wl_touch_add_listener(state->touch, &touch_listener, state);
    } else if (!have_touch && state->touch != NULL) {
        wl_touch_release(state->touch);
        state->touch = NULL;
    }
}

static void
seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)seat;
    (void)data;
    fprintf(stderr, "seat name: %s\n", name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};
```

The pattern is create-on-appear, release-on-disappear, guarded by a `NULL` check so a repeated `capabilities` event with unchanged bits does nothing. This is hotplug handling: unplugging a mouse sends a new `capabilities` without the pointer bit, and you must release the `wl_pointer` then. Replugging sends the bit again and you call `wl_seat_get_pointer` afresh. Holding a released pointer and calling it is a protocol error that kills the client. The seat object itself survives the whole session; only its children come and go.

The `name` handler only logs. On almost every machine it prints `seat0`. Do not branch on that string.

`main` needs two additions: the new headers and one XKB setup line before entering the event loop:

```c
#include <assert.h>
#include <string.h> /* memset */
#include <sys/mman.h>
#include <unistd.h> /* close */
#include <xkbcommon/xkbcommon.h>
```

`<stdio.h>` and `<stdbool.h>` are assumed from earlier chapters.

```c
state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
assert(state.xkb_context != NULL);
```

The context is layout-independent and lives as long as the client. Keymaps and states come and go underneath it as the compositor replaces layouts. The `assert` is appropriate here: without a context no keyboard input can ever be decoded, and continuing would only produce confusing empty logs.

Data flow for the whole chapter looks like this:

```
 compositor events            accumulators              frame dispatch
 ------------------           ------------              --------------

 pointer.enter/motion/
  button/axis/axis_source -+-> pointer_accum ------+
                           |   event_mask + axes[2] |-> pointer.frame -> printf
                           |                        |   + memset reset
 keyboard.keymap/enter/ -+--> xkb_keymap/state ------+-> printf immediately
  key/modifiers/leave  |    (no frame; self-contained)
                       |
 touch.down/up/motion/ -+--> touch_accum --------+
  shape/orientation/cancel |  points[10] + mask  |--> touch.frame -> printf
                                                    + valid=false reset
```

Pointer and touch accumulate and flush. Keyboard translates and prints immediately. Keep that asymmetry in mind; it explains why only two of the three devices need an accumulator struct.

## 12.2 Pointer: accumulate everything, print on frame

Pointer events arrive in bursts. A single flick of the mouse can produce `enter`, `motion`, `button`, two `axis` events, an `axis_source`, and an `axis_discrete` before the terminating `frame`. If you print or redraw inside each handler, you do the work five times and you observe torn intermediate states where the button is known but the position is stale. The fix is one struct plus one bitmask.

Define one bit per event type:

```c
enum pointer_event_mask {
    POINTER_EVENT_ENTER = 1 << 0,
    POINTER_EVENT_LEAVE = 1 << 1,
    POINTER_EVENT_MOTION = 1 << 2,
    POINTER_EVENT_BUTTON = 1 << 3,
    POINTER_EVENT_AXIS = 1 << 4,
    POINTER_EVENT_AXIS_SOURCE = 1 << 5,
    POINTER_EVENT_AXIS_STOP = 1 << 6,
    POINTER_EVENT_AXIS_DISCRETE = 1 << 7,
};

struct pointer_axis {
    bool valid;
    double value;
    int32_t discrete;
};

struct pointer_accum {
    uint32_t event_mask;
    uint32_t serial; /* last serial seen (enter/leave/button) */
    uint32_t time; /* last timestamp seen (motion/button/axis/stop) */
    double enter_x, enter_y;
    uint32_t enter_serial;
    uint32_t leave_serial;
    double x, y;
    uint32_t button, button_state;
    uint32_t button_serial;
    uint32_t axis_source;
    bool axis_stopped[2];
    struct pointer_axis axes[2]; /* 0 = vertical, 1 = horizontal */
};
```

Two axis slots are enough because the protocol defines exactly two axes: `WL_POINTER_AXIS_VERTICAL_SCROLL` (0) and `WL_POINTER_AXIS_HORIZONTAL_SCROLL` (1). Each slot records whether anything arrived in this burst (`valid`), the summed smooth scroll amount (`value`), and the summed discrete notch count (`discrete`). A trackpad finger swipe produces large `value` with zero `discrete`; a notched wheel produces both, because the compositor synthesizes discrete clicks for smooth sources so old clients keep working.

Each handler stores and sets a bit. None of them prints:

```c
static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
        uint32_t serial, struct wl_surface *surface,
        wl_fixed_t x, wl_fixed_t y)
{
    struct client_state *state = data;
    (void)pointer;
    (void)surface;
    state->pointer_accum.event_mask |= POINTER_EVENT_ENTER;
    state->pointer_accum.serial = serial;
    state->pointer_accum.enter_serial = serial;
    state->pointer_accum.enter_x = wl_fixed_to_double(x);
    state->pointer_accum.enter_y = wl_fixed_to_double(y);
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
        uint32_t serial, struct wl_surface *surface)
{
    struct client_state *state = data;
    (void)pointer;
    (void)surface;
    state->pointer_accum.event_mask |= POINTER_EVENT_LEAVE;
    state->pointer_accum.serial = serial;
    state->pointer_accum.leave_serial = serial;
}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
        uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    struct client_state *state = data;
    (void)pointer;
    state->pointer_accum.event_mask |= POINTER_EVENT_MOTION;
    state->pointer_accum.time = time;
    state->pointer_accum.x = wl_fixed_to_double(x);
    state->pointer_accum.y = wl_fixed_to_double(y);
}

static void
pointer_handle_button(void *data, struct wl_pointer *pointer,
        uint32_t serial, uint32_t time, uint32_t button, uint32_t btn_state)
{
    struct client_state *state = data;
    (void)pointer;
    state->pointer_accum.event_mask |= POINTER_EVENT_BUTTON;
    state->pointer_accum.serial = serial;
    state->pointer_accum.button_serial = serial;
    state->pointer_accum.time = time;
    state->pointer_accum.button = button;
    state->pointer_accum.button_state = btn_state;
}

static void
pointer_handle_axis(void *data, struct wl_pointer *pointer,
        uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct client_state *state = data;
    (void)pointer;
    assert(axis < 2);
    state->pointer_accum.event_mask |= POINTER_EVENT_AXIS;
    state->pointer_accum.time = time;
    state->pointer_accum.axes[axis].valid = true;
    /* Summing multiple axis events per frame; reference source overwrites instead. */
    state->pointer_accum.axes[axis].value += wl_fixed_to_double(value);
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *pointer,
        uint32_t axis_source)
{
    struct client_state *state = data;
    (void)pointer;
    state->pointer_accum.event_mask |= POINTER_EVENT_AXIS_SOURCE;
    state->pointer_accum.axis_source = axis_source;
}

static void
pointer_handle_axis_stop(void *data, struct wl_pointer *pointer,
        uint32_t time, uint32_t axis)
{
    struct client_state *state = data;
    (void)pointer;
    assert(axis < 2);
    state->pointer_accum.event_mask |= POINTER_EVENT_AXIS_STOP;
    state->pointer_accum.time = time;
    state->pointer_accum.axis_stopped[axis] = true;
    state->pointer_accum.axes[axis].valid = true;
}

static void
pointer_handle_axis_discrete(void *data, struct wl_pointer *pointer,
        uint32_t axis, int32_t discrete)
{
    struct client_state *state = data;
    (void)pointer;
    assert(axis < 2);
    state->pointer_accum.event_mask |= POINTER_EVENT_AXIS_DISCRETE;
    state->pointer_accum.axes[axis].valid = true;
    state->pointer_accum.axes[axis].discrete += discrete;
}
```

Note that `axis_stop` also marks its axis `valid`. A kinetic scroll that coasts to a halt sends `axis` events followed by `axis_stop` with no further value; without marking valid, the frame printer would have nothing to attach the "stopped" flag to. `axis_source`, `axis_stop`, `axis_discrete`, and `frame` all arrived in `wl_pointer` version 5, so binding version 5 or higher is required to see them.

The `frame` handler does all the reporting, then wipes the struct:

```c
static const char *
axis_name(uint32_t axis)
{
    return axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? "vertical" : "horizontal";
}

static const char *
axis_source_name(uint32_t source)
{
    switch (source) {
    case WL_POINTER_AXIS_SOURCE_WHEEL: return "wheel";
    case WL_POINTER_AXIS_SOURCE_FINGER: return "finger";
    case WL_POINTER_AXIS_SOURCE_CONTINUOUS: return "continuous";
    case WL_POINTER_AXIS_SOURCE_WHEEL_TILT: return "wheel-tilt";
    default: return "unknown";
    }
}

static void
pointer_handle_frame(void *data, struct wl_pointer *pointer)
{
    struct client_state *state = data;
    (void)pointer;
    struct pointer_accum *p = &state->pointer_accum;

    fprintf(stderr, "pointer frame @ %u\n", p->time);
    if (p->event_mask & POINTER_EVENT_ENTER)
        fprintf(stderr, "pointer enter: %.2f, %.2f (serial %u)\n",
            p->enter_x, p->enter_y, p->enter_serial);
    if (p->event_mask & POINTER_EVENT_LEAVE)
        fprintf(stderr, "pointer leave (serial %u)\n", p->leave_serial);
    if (p->event_mask & POINTER_EVENT_MOTION)
        fprintf(stderr, "pointer motion: %.2f, %.2f\n", p->x, p->y);
    if (p->event_mask & POINTER_EVENT_BUTTON)
        fprintf(stderr, "pointer button %u %s (serial %u)\n", p->button,
            p->button_state == WL_POINTER_BUTTON_STATE_PRESSED ?
            "pressed" : "released", p->button_serial);

    for (uint32_t i = 0; i < 2; i++) {
        if (!p->axes[i].valid)
            continue;
        fprintf(stderr, "pointer axis %s: value %.2f discrete %d",
            axis_name(i), p->axes[i].value, p->axes[i].discrete);
        if (p->event_mask & POINTER_EVENT_AXIS_SOURCE)
            fprintf(stderr, " source %s", axis_source_name(p->axis_source));
        if (p->axis_stopped[i])
            fprintf(stderr, " stopped");
        fprintf(stderr, "\n");
    }

    memset(p, 0, sizeof(*p));
}
```

The `memset` at the end is the whole trick. It clears the mask, zeroes the axis values, and resets `valid` in one line. Forgetting it duplicates every log line on the next frame; clearing at the top instead of the bottom drops the first burst if two frames are queued. Accumulate during the burst, consume on `frame`, reset last.

Serials are kept because they authorize later requests: the enter serial feeds `wl_pointer.set_cursor`, and button or leave serials feed `xdg_popup`, `xdg_toplevel.move`, `resize`, and `wl_data_device.start_drag`. Timestamps only order and log the burst, printed in the `pointer frame @ %u` header.

> **Misconception callout: "should I handle each event instantly?" No.**
>
> Beginners put application logic in `motion` or `button` directly because each handler looks self-contained. That works until the compositor sends motion plus button plus axis in one burst and the app redraws three times, or reads button state before the matching motion has arrived. The protocol guarantees only that a `frame` terminates each burst, not that each event is meaningful alone. So store in the handler, decide in `frame`. Keyboard is the exception: its events carry no `frame` and each one stands alone.

## 12.3 Keyboard: keymap, keysyms, and modifiers

Keyboard handling has no accumulator because there is no `wl_keyboard.frame`. Each event is complete on arrival, but each key is meaningless without the XKB state built from earlier `keymap` and `modifiers` events. Setup order matters: context once, keymap on demand, state update before translation.

Hotplug mirrors the pointer. On capability appearance, get the keyboard and attach listeners; on disappearance, release and null it. `wl_keyboard_release` exists since version 3; with an older bind, skip the release and just null the pointer (this client binds the seat at 7, so release is always available):

```c
if (have_keyboard && state->keyboard == NULL) {
    state->keyboard = wl_seat_get_keyboard(state->seat);
    wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
} else if (!have_keyboard && state->keyboard != NULL) {
    wl_keyboard_release(state->keyboard);
    state->keyboard = NULL;
}
```

The `keymap` handler receives a file descriptor and a size. Only one format exists, `WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1`. Map the file, compile it from a string, build a new state, and replace the old pair:

```c
static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
        uint32_t format, int fd, uint32_t size)
{
    struct client_state *state = data;
    (void)keyboard;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        state->xkb_context, map,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (keymap == NULL)
        return;

    struct xkb_state *xkb_state = xkb_state_new(keymap);
    if (xkb_state == NULL) {
        xkb_keymap_unref(keymap);
        return;
    }

    xkb_state_unref(state->xkb_state);
    xkb_keymap_unref(state->xkb_keymap);
    state->xkb_keymap = keymap;
    state->xkb_state = xkb_state;
}
```

Three details deserve attention. First, `MAP_SHARED` versus `MAP_PRIVATE` does not matter here because the mapping is read-only and immediately unmapped; either flag works, and you will see both in real clients. Second, the fd must be closed on every path, including the wrong-format early return, or the client leaks an fd per layout switch. Third, the unref-old dance handles runtime keymap replacement: the compositor may send a new keymap when the user switches layout, and the old keymap and state must be unreffed after the new ones are ready. Unreffing `NULL` is safe in xkbcommon, so no `NULL` check is needed before the swap.

Focus events bracket typing. `enter` carries the focused surface, a serial, and an array of already-held scancodes:

```c
static void
keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface,
        struct wl_array *keys)
{
    struct client_state *state = data;
    (void)keyboard;
    (void)surface;
    state->last_keyboard_serial = serial;
    fprintf(stderr, "keyboard enter\n");

    /* enter can arrive before keymap; without state there is nothing to translate. */
    if (!state->xkb_state)
        return;

    uint32_t *key;
    wl_array_for_each(key, keys) {
        xkb_keycode_t code = *key + 8;
        char name[64];
        xkb_keysym_t sym =
            xkb_state_key_get_one_sym(state->xkb_state, code);
        xkb_keysym_get_name(sym, name, sizeof(name));
        char text[64];
        xkb_state_key_get_utf8(state->xkb_state, code,
            text, sizeof(text));
        fprintf(stderr, "  already pressed: sym %s text '%s'\n",
            name, text);
    }
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface)
{
    struct client_state *state = data;
    (void)keyboard;
    (void)surface;
    state->last_keyboard_serial = serial;
    fprintf(stderr, "keyboard leave\n");
}
```

The `wl_array_for_each` loop handles Alt+Tab correctly: when focus arrives with Alt already held, that scancode is in the array, and the client seeds its held-key set instead of believing no modifier is down. The `+ 8` offset converts evdev scancodes, which start at zero, to XKB keycodes, which start at eight. Forgetting it shifts every key by eight positions and produces nonsense symbols. The serial is saved into `last_keyboard_serial` for any future request that needs keyboard focus authentication; this logging client otherwise discards it. Both `enter` and `key` can arrive before the first `keymap`, when `xkb_state` is still `NULL`, so each guards before translating - without the guard the client segfaults on startup focus.

Individual keys follow the same translation:

```c
static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, uint32_t time, uint32_t key, uint32_t key_state)
{
    struct client_state *state = data;
    (void)keyboard;
    (void)time;
    state->last_keyboard_serial = serial;

    /* key can arrive before keymap; without state there is nothing to translate. */
    if (!state->xkb_state)
        return;

    xkb_keycode_t code = key + 8;
    char name[64];
    xkb_keysym_t sym = xkb_state_key_get_one_sym(state->xkb_state, code);
    xkb_keysym_get_name(sym, name, sizeof(name));
    char text[128];
    int n = xkb_state_key_get_utf8(state->xkb_state, code,
        text, sizeof(text));

    fprintf(stderr, "key %s: sym %s (%u) text '%.*s'\n",
        key_state == WL_KEYBOARD_KEY_STATE_PRESSED ? "pressed" : "released",
        name, sym, n, text);
}
```

Only `pressed` and `released` exist on the wire; the compositor never repeats for you. Client-side repeat is synthesized from `repeat_info` (rate and delay), covered in the exercises. Seeing `pressed` without a matching `released` after a focus switch reveals a missing leave handler.

Modifiers are a pure passthrough, but they must run before the next `key` translation:

```c
static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
        uint32_t mods_locked, uint32_t group)
{
    struct client_state *state = data;
    (void)keyboard;
    state->last_keyboard_serial = serial;
    if (!state->xkb_state)
        return;
    xkb_state_update_mask(state->xkb_state,
        mods_depressed, mods_latched, mods_locked, 0, 0, group);
}
```

If this call lags one event behind, Shift+letter prints `s` instead of `S` on exactly the first keypress after Shift goes down - a classic off-by-one that looks like flaky translation but is really stale modifier state.

The last event, `repeat_info` (since version 4), carries rate and delay for client-side key repeat and is left as an exercise at the end of this chapter. The design notes there explain why the compositor never repeats for you.

## 12.4 Touch: IDs, shapes, and cancel-everything

Touch resembles the pointer - bursts terminated by `frame` - but fingers are identified by numeric IDs rather than by hover, and the whole gesture can be revoked at once. The accumulator is an array of points plus a top-level mask, serial, and timestamp:

```c
/* Bit order here is intentional and local-only; it need not match
 * protocol event numbering. */
enum touch_event_mask {
    TOUCH_EVENT_DOWN = 1 << 0,
    TOUCH_EVENT_UP = 1 << 1,
    TOUCH_EVENT_MOTION = 1 << 2,
    TOUCH_EVENT_SHAPE = 1 << 3,
    TOUCH_EVENT_ORIENTATION = 1 << 4,
    TOUCH_EVENT_CANCEL = 1 << 5,
};

struct touch_point {
    bool valid;
    int32_t id;
    uint32_t mask; /* which of DOWN/UP/MOTION/SHAPE/ORIENTATION touched it */
    double x, y;
    double major, minor;
    double orientation;
};

struct touch_accum {
    uint32_t event_mask;
    uint32_t time;
    uint32_t serial;
    struct touch_point points[10];
};
```

Ten slots is a deliberate headroom choice. Nothing in the protocol caps simultaneous touches, but real hardware reports at most eight to ten, and ten fingers already cover two full hands. A fixed array avoids allocation inside the event burst, and the full-table case is handled by dropping with a warning rather than crashing.

A helper finds the slot for an ID: an existing slot with a matching ID wins, otherwise the first free slot, otherwise `NULL`:

```c
static struct touch_point *
get_touch_point(struct client_state *state, int32_t id)
{
    for (size_t i = 0; i < 10; i++) {
        if (state->touch_accum.points[i].valid &&
                state->touch_accum.points[i].id == id)
            return &state->touch_accum.points[i];
    }
    for (size_t i = 0; i < 10; i++) {
        if (!state->touch_accum.points[i].valid) {
            state->touch_accum.points[i].valid = true;
            state->touch_accum.points[i].id = id;
            return &state->touch_accum.points[i];
        }
    }
    return NULL;
}
```

The `down`, `up`, and `motion` handlers accumulate position and set both the top-level and per-point masks. Each also records the burst timestamp and serial at the top level for the frame printer:

```c
static void
touch_handle_down(void *data, struct wl_touch *touch,
        uint32_t serial, uint32_t time, struct wl_surface *surface,
        int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    struct client_state *state = data;
    (void)touch;
    (void)surface;
    struct touch_point *point = get_touch_point(state, id);
    if (point == NULL) {
        fprintf(stderr, "too many touch points\n");
        return;
    }
    state->touch_accum.event_mask |= TOUCH_EVENT_DOWN;
    state->touch_accum.serial = serial;
    state->touch_accum.time = time;
    point->mask |= TOUCH_EVENT_DOWN;
    point->x = wl_fixed_to_double(x);
    point->y = wl_fixed_to_double(y);
}

static void
touch_handle_up(void *data, struct wl_touch *touch,
        uint32_t serial, uint32_t time, int32_t id)
{
    struct client_state *state = data;
    (void)touch;
    struct touch_point *point = get_touch_point(state, id);
    if (point == NULL)
        return;
    state->touch_accum.event_mask |= TOUCH_EVENT_UP;
    state->touch_accum.serial = serial;
    state->touch_accum.time = time;
    point->mask |= TOUCH_EVENT_UP;
}

static void
touch_handle_motion(void *data, struct wl_touch *touch,
        uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    struct client_state *state = data;
    (void)touch;
    struct touch_point *point = get_touch_point(state, id);
    if (point == NULL)
        return;
    state->touch_accum.event_mask |= TOUCH_EVENT_MOTION;
    state->touch_accum.time = time;
    point->mask |= TOUCH_EVENT_MOTION;
    point->x = wl_fixed_to_double(x);
    point->y = wl_fixed_to_double(y);
}
```

> **A note on the published text:** early printings of this example contain a copy-paste slip in `touch_handle_down`, OR-ing `TOUCH_EVENT_UP` instead of `TOUCH_EVENT_DOWN` into the top-level mask. With that typo a fresh finger press never sets the `DOWN` bit, so the frame printer reports the point as a motion or ignores it. If your log shows `down` bursts with no down line, check that one line first. The code above shows the corrected version.

`cancel` is different: it carries no ID because it revokes the entire sequence. Set the flag and let `frame` drop everything:

```c
static void
touch_handle_cancel(void *data, struct wl_touch *touch)
{
    struct client_state *state = data;
    (void)touch;
    state->touch_accum.event_mask |= TOUCH_EVENT_CANCEL;
}
```

`shape` and `orientation` (both since version 6) refine a single point with the contact ellipse diameters and its rotation. They arrive inside the same burst as `down` or `motion` and are consumed on the same `frame`. Timestamps throughout touch (and pointer/keyboard) are milliseconds with an arbitrary epoch, not wall-clock time - only for ordering and relative measurement. `orientation` is in degrees from the Y axis.

```c
static void
touch_handle_shape(void *data, struct wl_touch *touch,
        int32_t id, wl_fixed_t major, wl_fixed_t minor)
{
    struct client_state *state = data;
    (void)touch;
    struct touch_point *point = get_touch_point(state, id);
    if (point == NULL)
        return;
    state->touch_accum.event_mask |= TOUCH_EVENT_SHAPE;
    point->mask |= TOUCH_EVENT_SHAPE;
    point->major = wl_fixed_to_double(major);
    point->minor = wl_fixed_to_double(minor);
}

static void
touch_handle_orientation(void *data, struct wl_touch *touch,
        int32_t id, wl_fixed_t orientation)
{
    struct client_state *state = data;
    (void)touch;
    struct touch_point *point = get_touch_point(state, id);
    if (point == NULL)
        return;
    state->touch_accum.event_mask |= TOUCH_EVENT_ORIENTATION;
    point->mask |= TOUCH_EVENT_ORIENTATION;
    point->orientation = wl_fixed_to_double(orientation);
}
```

The frame printer walks all ten slots and reports only valid ones, then releases every slot:

```c
static void
touch_handle_frame(void *data, struct wl_touch *touch)
{
    struct client_state *state = data;
    (void)touch;

    if (state->touch_accum.event_mask & TOUCH_EVENT_CANCEL) {
        fprintf(stderr, "touch cancel: dropping all points\n");
        for (size_t i = 0; i < 10; i++)
            state->touch_accum.points[i].valid = false;
        state->touch_accum.event_mask = 0;
        return;
    }

    for (size_t i = 0; i < 10; i++) {
        struct touch_point *p = &state->touch_accum.points[i];
        if (!p->valid || p->mask == 0)
            continue;
        fprintf(stderr, "touch point %d @ %.2f, %.2f:",
            p->id, p->x, p->y);
        if (p->mask & TOUCH_EVENT_DOWN)
            fprintf(stderr, " down");
        if (p->mask & TOUCH_EVENT_UP)
            fprintf(stderr, " up");
        if (p->mask & TOUCH_EVENT_MOTION)
            fprintf(stderr, " motion");
        if (p->mask & TOUCH_EVENT_SHAPE)
            fprintf(stderr, " shape %.2fx%.2f", p->major, p->minor);
        if (p->mask & TOUCH_EVENT_ORIENTATION)
            fprintf(stderr, " orientation %.1f", p->orientation);
        fprintf(stderr, " (time %u)\n", state->touch_accum.time);
        /* A point that went up is finished; a point that only
         * moved stays valid for the next burst. */
        if (p->mask & TOUCH_EVENT_UP)
            p->valid = false;
        p->mask = 0;
    }
    state->touch_accum.event_mask = 0;
}
```

The reset here is per-point rather than a single `memset`, because touch points outlive one frame: a finger held across three bursts is `down` once, `motion` many times, `up` once. Clearing `valid` for every point on every frame would forget held fingers. So the mask is cleared each frame but `valid` survives until `up` or `cancel`. Compare with the pointer, where the whole struct is wiped: the pointer has at most one position, while touch has a set of leases with independent lifetimes.

## 12.5 Building and reading the log

The new dependencies are `wayland-client` as before plus `xkbcommon`. With pkg-config the build line is:

```
cc -o input-log main.c xdg-shell-protocol.c \
    $(pkg-config --cflags --libs wayland-client xkbcommon)
```

The generated `xdg-shell-protocol.c` comes from `wayland-scanner` as in Chapter 9; nothing about input changes that step. Run the binary inside a compositor - Weston, Mutter, KWin, or Sway - with a terminal alongside so you can see stderr. Move the pointer across the window, click both buttons, scroll with wheel and trackpad, type with and without Shift, and, on a touchscreen, press and drag one and two fingers.

A healthy session reads like the burst diagram: several store lines collapsed into one printed burst. Pointer motion floods - dozens of motion-plus-frame groups per second - while keyboard lines appear singly with keysym plus text. Touch lines cluster by ID. If the log shows every pointer event twice, the accumulator was not reset. If Shift sticks, `modifiers` is applied after translation instead of before. If held fingers never vanish, `cancel` is ignored or `up` fails to clear `valid`.

## Recap

- `client_state` owns the seat, its three devices, the XKB context, keymap, and state, plus one accumulator each for pointer and touch. Registry code binds `wl_seat` at version 7 and attaches the seat listener immediately.
- `capabilities` hotplugs devices with create-on-appear and release-on-disappear; `name` only logs. Guard each branch with a `NULL` check and never call a released device.
- Pointer input accumulates behind an eight-bit `event_mask` with a two-slot `axes[2]` array holding `valid`, `value`, and `discrete`. Handlers store and OR; `frame` prints enter position, leave, motion, button press or release, and per-valid-axis name, value, discrete count, source, and stopped flag, then `memset` resets.
- Keyboard input compiles the `keymap` fd with `mmap` and `xkb_keymap_new_from_string`, swaps keymap and state with unref-old semantics, translates scancode `+ 8` to keysym name and UTF-8 in both `enter` (via `wl_array_for_each`) and `key`, forwards `modifiers` through `xkb_state_update_mask` before translation, and logs `leave` to drop held keys.
- Touch input tracks ten points by ID with a `get_touch_point` helper returning the existing, first free, or `NULL` slot. `down`, `up`, `motion`, `shape`, and `orientation` accumulate per-point; `cancel` drops everything; `frame` prints per-valid-point verbs and geometry, clears `mask`, and clears `valid` only on `up` or `cancel`.
- Watch for the published `down`-handler typo that ORs `UP` instead of `DOWN`, and build with `wayland-client` plus `xkbcommon`.

## Exercises

1. **Pretty-print axis sources.** Extend the pointer frame printer to name all four axis sources (`wheel`, `finger`, `continuous`, `wheel-tilt`) and to distinguish a discrete wheel click from a smooth swipe in the log. Scroll with a notched wheel, a trackpad two-finger swipe, and, if available, a tilt wheel or high-resolution wheel. Confirm that discrete counts appear even for smooth sources, that `axis_stop` arrives when fingers lift, and that no axis line prints without its `valid` bit set.
2. **Implement key repeat from `repeat_info`.** Store the rate and delay from `repeat_info` (rate 0 means disabled), start a timer on `key pressed` using `timerfd` or your event loop, re-dispatch the stored keysym every `1000 / rate` milliseconds after the initial delay, and cancel on `key released` or `keyboard leave`. Design notes: repeat is per-key, not per-keyboard - only the most recently pressed key repeats; the compositor never repeats for you; and focus loss must cancel the timer or keys repeat into the wrong window. Test with rate 0, with a held letter, and with focus switched mid-hold.
