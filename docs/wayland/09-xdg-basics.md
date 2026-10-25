# Chapter 9: XDG Shell Basics - Your First Window

> **Objectives:** Explain why window policy lives outside core Wayland; bind `xdg_wm_base` version 1 and answer `ping` with `pong`; wrap a `wl_surface` in an `xdg_surface` role; implement the `configure` / `ack_configure` handshake; set title, app ID, and window geometry correctly; run the 8-step zero-to-window recipe to show a 640x480 checkerboard; build with `wayland-scanner`; read the exchange under `WAYLAND_DEBUG=1`.

## 9.1 Core has no windows on purpose

Core Wayland knows surfaces, buffers, and seats. It deliberately does not know what a "window" is: no titlebars, no minimize, no taskbars, no rule for who goes on top. That policy lives in an extension so different desktop environments can implement different behaviours on the same wire protocol.

The standard desktop extension is `xdg-shell`. Its interface definition ships with `wayland-protocols` as `stable/xdg-shell.xml`. "Stable" matters: unlike `unstable/` or `staging/` namespaces, this XML is frozen for backward compatibility. A client written against version 1 keeps working against a compositor that speaks version 7, because the compositor must preserve every version 1 request, event, and semantic.

Think of the split this way. `wl_compositor` gives you a blank stacking rectangle. `xdg-shell` gives that rectangle a job description: this one is a top-level window with a title and an app ID, that one is a popup menu tied to a parent, another is positioned by a `xdg_positioner`. The compositor enforces the description, the client draws the pixels, and neither has to agree on decoration style.

If you come from X11, forget `XMapWindow` and ICCCM hints. In Wayland there is no map call. There is a role assignment, an empty commit that announces the role, a configure round trip where the compositor answers with a size, and only then a buffer attach. Every first-window bug is a shortcut taken somewhere in that sequence.

## 9.2 `xdg_wm_base`: the window-manager global

The entry point is a single global, `xdg_wm_base`. It is the factory for every window and popup, and the channel for liveness checks.

Bind it at version 1 unless you need a newer feature such as `configure_bounds` (version 4) or `wm_capabilities` (version 5). Version 1 gives you `get_xdg_surface`, `create_positioner`, `pong`, and `ping`:

```c
static struct xdg_wm_base *wm_base = NULL;

/* in registry_global: */
if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    wm_base = wl_registry_bind(registry, name,
        &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
}
```

The registry pattern is identical to `wl_compositor` (bind version 4) and `wl_shm` (bind version 1) from the previous chapter. Request these versions as floors: the version actually bound is clamped to the lower of what you ask and what the compositor advertises, so the code must run against the version it requested and never assume anything newer. One roundtrip after adding the registry listener populates all three pointers. Check each for `NULL` before continuing; a nested compositor or test harness may genuinely lack `xdg_wm_base`.

Liveness is a `ping` event carrying a serial. The client must answer with `pong` and the same serial, or the compositor assumes it hung and may show a "not responding" dialog or kill it:

```c
static void
wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void) data;
    xdg_wm_base_pong(wm_base, serial);
}
```

That is the whole handler. Add it with `xdg_wm_base_add_listener` inside `registry_global`'s `xdg_wm_base` branch, before the roundtrip: a `ping` can arrive during `wl_display_roundtrip`, and a listener added only after the roundtrip would miss it and fall into the unresponsive path. It costs five lines and saves hours of "why did my window vanish after ten seconds" debugging. The serial has no other meaning; do not confuse it with configure serials. Miss the `pong` for too long and the compositor raises the `xdg_wm_base` `unresponsive` fatal error; calling `xdg_wm_base.destroy` while any `xdg_surface` created from it still lives raises `defunct_surfaces`.

`xdg_wm_base` also has a `destroy` destructor. Call it at shutdown after destroying every `xdg_surface` created from it.

## 9.3 From `wl_surface` to `xdg_surface`: assigning the role

A `wl_surface` starts role-less. Chapter 8 showed it can display SHM pixels, but the compositor still does not know whether it is a window, a cursor, or a layer. `get_xdg_surface` assigns the window role:

```c
struct wl_surface *surface = wl_compositor_create_surface(compositor);
struct xdg_surface *xdg_surface =
    xdg_wm_base_get_xdg_surface(wm_base, surface);
```

The generated signature is `get_xdg_surface(new id xdg_surface, surface)`: a new `xdg_surface` object bound to an existing `wl_surface`. After this call the surface has exactly one role. Giving it a second role, for example also attaching it as a cursor or subsurface, raises the `xdg_wm_base` `role` fatal error and the compositor disconnects the client. If you want the same pixels twice, share the `wl_buffer`, never the surface.

`xdg_surface` itself is not yet a window. It is a generic shell surface that must become either a toplevel (`get_toplevel`) or a popup (`get_popup`). This chapter uses toplevel only; popups, with their `xdg_positioner` placement and grab semantics, are deferred to Chapter 13:

```c
struct xdg_toplevel *toplevel =
    xdg_surface_get_toplevel(xdg_surface);
```

From now on two objects describe one window. The `wl_surface` carries pixels through `attach` / `damage` / `commit`. The `xdg_surface` carries shell state through `configure` / `ack_configure`. The `xdg_toplevel` carries window metadata: title, app ID, size limits, fullscreen and maximized state. Keep all three pointers in your state struct until shutdown.

## 9.4 `configure` / `ack_configure` and window geometry

The compositor, not the client, has the final say on window size. It may apply output scale, layout policy, saved session geometry, or tiling constraints. So sizing is a handshake, not an assignment.

The compositor sends `xdg_surface.configure` with a serial number. The client should ack the most recent serial it has seen with `xdg_surface.ack_configure(serial)`: if several configures arrive before the client responds, acking only the last one is legal and expected, and the acked state becomes the agreed state. Acking the same serial twice, acking an older serial after a newer one, or acking a serial the compositor never sent raises the `xdg_surface` `invalid_serial` fatal error (value 4). Until the client acks, the compositor considers the suggested state provisional. Until the compositor sends a configure, the client has no agreed state at all and must not assume its preferred size won. Attaching a buffer before the first configure is the `xdg_surface` `unconfigured_buffer` fatal error (value 3), which is why the empty commit in section 9.6 carries no pixels.

In practice the handler looks like this:

```c
static void
xdg_surface_configure(void *data,
    struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct state *st = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    draw_frame(st);
}
```

Ack first, then draw using the latest toplevel size (section 9.5). The ack may be sent together with the buffer commit in the same `wl_display` flush; libwayland queues both requests and writes them on the next flush or roundtrip.

A related request is `set_window_geometry(x, y, width, height)`. It tells the compositor which part of the surface is the "real" window. The distinction matters when the client draws extra padding for drop shadows, rounded corners, or glow effects. Without a geometry hint the compositor would treat the shadow pixels as clickable and would tile the window including the shadow margin, which looks broken. With the hint:

```c
xdg_surface_set_window_geometry(xdg_surface, 0, 0, width, height);
```

the compositor positions, tiles, and hit-tests only the given rectangle, while still displaying the full buffer including the decorative margin. Set geometry to the clickable area, not the buffer size, whenever they differ. For an opaque rectangle with no shadows the two are equal and the call can be omitted. `set_window_geometry` is double-buffered state: it takes effect on the next `wl_surface.commit`, like buffer attachment. The rectangle is clamped to the surface bounds, and a width or height of zero or less raises the `invalid_size` fatal error. Once set, the geometry cannot be unset, only replaced with a new rectangle.

Attaching a null buffer unmaps the toplevel; to show it again, commit empty and wait for a fresh configure before attaching a new buffer.

## 9.5 Toplevel: title, app ID, size, and close

`xdg_toplevel` is where a window becomes recognizable to the user. Two strings matter:

```c
xdg_toplevel_set_title(toplevel, "Checker");
xdg_toplevel_set_app_id(toplevel, "org.example.checker");
```

`set_title` is the human label shown in the titlebar and the Alt-Tab switcher. Change it freely; "Untitled Document - Editor" updating to "notes.txt - Editor" is exactly what it is for. `set_app_id` is the machine class used by the dock, taskbar grouping, and compositor rules to decide "these three windows belong to the same app." Use a reverse-DNS ID such as `org.example.checker`, matching your `.desktop` file: by convention it is a D-Bus-style service name the launcher uses to group windows. Unlike the initial handshake state, it may be updated after the window is mapped, but changing it mid-run confuses grouping, so set it before the first commit and leave it unless you mean it.

Sizing arrives on `xdg_toplevel.configure(width, height, states)`:

```c
static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
    int32_t width, int32_t height, struct wl_array *states)
{
    (void) toplevel;
    (void) states;
    struct state *st = data;
    if (width > 0 && height > 0) {
        st->width = width;
        st->height = height;
    }
}
```

Zero means "no opinion, client picks." Non-zero is a strong suggestion the client should honor; ignoring it breaks tiling and maximization. The `states` array lists hints: maximized, fullscreen, resizing, and activated exist since version 1, the `tiled_left` / `tiled_right` / `tiled_top` / `tiled_bottom` states arrived in version 2, and `suspended` in version 6. Related newer features live nearby: the `configure_bounds` event (version 4) caps the size a client should pick, and `wm_capabilities` (version 5) tells the client which compositor actions are available. A client bound at version 1 never sees any of the newer states or events, which is why binding version 1 keeps the code in this chapter simple. This example records only size. A fuller client would check for maximized and draw without rounded corners, or throttle animation when suspended.

Closing is an event, not a request. When the user clicks the close button or presses Alt-F4, the compositor sends `xdg_toplevel.close`. The client should exit its main loop and tear down:

```c
static void
xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void) toplevel;
    struct state *st = data;
    st->closed = true;
}
```

There is no `close` request to send back. Destroy the toplevel, the `xdg_surface`, and the `wl_surface`, then disconnect.

## 9.6 Zero to window in 8 steps

The full recipe, with the step most beginners skip highlighted:

1. **Connect.** `wl_display_connect(NULL)` respects `WAYLAND_DISPLAY` and returns the display connection.
2. **Bind globals.** Get the registry, add a listener for `wl_compositor` (version 4), `wl_shm` (version 1), and `xdg_wm_base` (version 1), then `wl_display_roundtrip` so the globals list arrives. Abort with a clear message if any is `NULL`.
3. **Create the surface.** `wl_compositor_create_surface` gives the blank endpoint.
4. **Wrap it.** `xdg_wm_base_get_xdg_surface` assigns the shell role, then `xdg_surface_get_toplevel` makes it a top-level window. Attach `xdg_surface` and `xdg_toplevel` listeners now.
5. **Label it.** `xdg_toplevel_set_title` and `xdg_toplevel_set_app_id` before the first commit so the compositor never shows an unlabeled window.
6. **Empty commit.** `wl_surface_commit` with no buffer attached. This commit carries only role and metadata, no pixels. It is what prompts the compositor to compute layout and reply with `configure`.
7. **Configure, ack, draw, commit.** On `xdg_surface.configure`, ack the serial, draw a buffer at the configured size, then `attach` / `damage` / `commit`.
8. **Dispatch until closed.** Loop `wl_display_dispatch` until the `closed` flag set by `xdg_toplevel.close` becomes true, then destroy in reverse order and disconnect.

The empty commit in step 6 deserves a diagram, because it feels wrong the first time:

```
client                                compositor
  |-- xdg_wm_base.get_xdg_surface --->|
  |-- xdg_surface.get_toplevel ------>|
  |-- xdg_toplevel.set_title -------->|
  |-- xdg_toplevel.set_app_id ------->|
  |-- wl_surface.commit (NULL buffer)>|  no pixels yet,
  |                                   |  just "I am a window"
  |                                   |-- layout, pick size
  |<-- xdg_toplevel.configure(w,h) --|
  |<-- xdg_surface.configure(serial)-|
  |-- xdg_surface.ack_configure ----->|
  |-- wl_surface.attach(buffer) ----->|
  |-- wl_surface.damage+commit ------>|  first pixels
  |<-- wl_buffer.release -------------|  compositor done (later)
```

The order inside one configure sequence is load-bearing: role-specific configures (`xdg_toplevel.configure`, or `xdg_popup.configure` in Chapter 13) arrive first and the terminating `xdg_surface.configure` comes last, so by the time the surface handler runs, `st->width` and `st->height` already hold the new size and drawing there is safe.

Without the empty commit the compositor has nothing to configure, so no `configure` event ever arrives, so a client that waits for configure before drawing waits forever, and a client that draws before configure draws at a size the compositor never agreed to.

> **Misconception: "Draw immediately after create, then commit."**
> No. After `create_surface` / `get_xdg_surface` / `get_toplevel` you own a role but have no agreed size. Drawing now guesses. The compositor may tile you at half width, scale you for HiDPI, or restore a maximized state, and your 640x480 buffer will be stretched, cropped, or rejected. Commit empty first, wait for `configure`, ack the serial, then draw at the configured size (or your default if width and height are zero). That one wait is the difference between a window that survives tiling and one that only works floating on your machine.

## 9.7 Complete client: 640x480 checkerboard

Below is a complete, compilable client. It opens one window, draws a gray checkerboard, handles resize configures, answers pings, and exits cleanly when the compositor sends `close`. It reuses the SHM helpers from Chapter 8 and allocates a fresh pool per frame for clarity: create file, `mmap`, create pool, create buffer, destroy pool, close file descriptor, draw, `munmap`, attach, damage, commit. The buffer destroys itself on `release`.

```c
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

struct state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    int width;
    int height;
    bool closed;
};

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

static void
wl_buffer_release(void *data, struct wl_buffer *buffer)
{
    (void) data;
    wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = wl_buffer_release,
};

static void
draw_frame(struct state *st)
{
    /* Safe: the terminating xdg_surface.configure runs after
       xdg_toplevel.configure in the same sequence (section 9.6),
       so st->width/height already hold the new size here, even
       though they are written by a different listener. */
    size_t stride = (size_t) st->width * 4;
    size_t size = stride * (size_t) st->height;
    int fd = allocate_shm_file(size);
    if (fd < 0) {
        fprintf(stderr, "shm file failed\n");
        return;
    }
    void *map = mmap(NULL, size,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap failed\n");
        close(fd);
        return;
    }
    uint8_t *data = map;
    struct wl_shm_pool *pool =
        wl_shm_create_pool(st->shm, fd, size);
    if (!pool) {
        fprintf(stderr, "create_pool failed\n");
        munmap(map, size);
        close(fd);
        return;
    }
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
        st->width, st->height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!buffer) {
        fprintf(stderr, "create_buffer failed\n");
        munmap(map, size);
        return;
    }

    uint32_t *px = (uint32_t *) data;
    for (int y = 0; y < st->height; ++y)
        for (int x = 0; x < st->width; ++x)
            px[y * st->width + x] = ((x / 32 + y / 32) % 2)
                ? 0xFF202020 : 0xFFC0C0C0;
    munmap(data, size);

    wl_buffer_add_listener(buffer, &buffer_listener, NULL);
    wl_surface_attach(st->surface, buffer, 0, 0);
    wl_surface_damage(st->surface, 0, 0, st->width, st->height);
    wl_surface_commit(st->surface);
}

static void
xdg_surface_configure(void *data,
    struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct state *st = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    draw_frame(st);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
    int32_t width, int32_t height, struct wl_array *states)
{
    (void) toplevel;
    (void) states;
    struct state *st = data;
    if (width > 0 && height > 0) {
        st->width = width;
        st->height = height;
    }
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void) toplevel;
    struct state *st = data;
    st->closed = true;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void
wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void) data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void
registry_global(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    (void) version;
    struct state *st = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        st->compositor = wl_registry_bind(registry, name,
            &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        st->shm = wl_registry_bind(registry, name,
            &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        st->wm_base = wl_registry_bind(registry, name,
            &xdg_wm_base_interface, 1);
        /* Listen before the roundtrip: a ping can arrive during
           wl_display_roundtrip, and a later add_listener would
           miss it and trip the unresponsive path. */
        xdg_wm_base_add_listener(st->wm_base, &wm_base_listener, st);
    }
}

static void
registry_global_remove(void *data, struct wl_registry *registry,
    uint32_t name)
{
    (void) data;
    (void) registry;
    (void) name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int
main(void)
{
    struct state st;
    memset(&st, 0, sizeof(st));
    st.width = 640;
    st.height = 480;

    st.display = wl_display_connect(NULL);
    if (!st.display) {
        fprintf(stderr, "no display\n");
        return 1;
    }
    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &registry_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.compositor || !st.shm || !st.wm_base) {
        fprintf(stderr, "missing globals\n");
        return 1;
    }
    /* Ping listener was already added in registry_global. */

    st.surface = wl_compositor_create_surface(st.compositor);
    st.xdg_surface =
        xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
    xdg_surface_add_listener(st.xdg_surface,
        &xdg_surface_listener, &st);
    st.toplevel = xdg_surface_get_toplevel(st.xdg_surface);
    xdg_toplevel_add_listener(st.toplevel,
        &xdg_toplevel_listener, &st);

    xdg_toplevel_set_title(st.toplevel, "Checker");
    xdg_toplevel_set_app_id(st.toplevel, "org.example.checker");
    wl_surface_commit(st.surface);

    while (!st.closed && wl_display_dispatch(st.display) != -1)
        ;

    /* A buffer still awaiting release at this point leaks: release
       is what destroys it, and none arrives after disconnect. Real
       code tracks the current buffer and destroys it here. */
    xdg_toplevel_destroy(st.toplevel);
    xdg_surface_destroy(st.xdg_surface);
    wl_surface_destroy(st.surface);
    xdg_wm_base_destroy(st.wm_base);
    wl_shm_destroy(st.shm);
    wl_compositor_destroy(st.compositor);
    wl_registry_destroy(st.registry);
    wl_display_disconnect(st.display);
    return 0;
}
```

Note the deliberate simplifications. Each frame builds a new pool instead of reusing one, which wastes setup overhead but keeps lifetime reasoning trivial: pool destroyed right after buffer creation (the buffer holds its own reference), file descriptor closed, mapping unmapped after pixels are written. The compositor keeps its own mapping, so unmapping client-side is safe. A production client would keep a pool pair and wait for `release` before repainting; here `release` just destroys the buffer, and a buffer still in flight when `close` arrives leaks. Resizing works because `draw_frame` reads the latest `st->width` and `st->height` stored by the toplevel configure handler. Compared with a minimal first-window listing, three things were added here: the explicit `wl_surface_damage` call, redrawing at each configured size instead of a fixed 640x480, and exiting cleanly on `xdg_toplevel.close`.

## 9.8 Building with `wayland-scanner`

The `xdg-shell-client-protocol.h` header in the listing is not shipped with libwayland. Generate it from the installed protocol XML, plus the private implementation file, then compile both translation units:

```sh
wayland-scanner client-header \
    < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    > xdg-shell-client-protocol.h
wayland-scanner private-code \
    < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    > xdg-shell-protocol.c
cc -o checker checker.c xdg-shell-protocol.c -lwayland-client -lrt
```

`client-header` emits the structs, listener types, and inline request wrappers (`xdg_wm_base_get_xdg_surface`, `xdg_surface_ack_configure`, and friends). `private-code` emits the interface descriptors the proxy marshaller needs. Omitting the second file fails at link time with an undefined reference to `xdg_wm_base_interface` (and friends), so always compile both. `-lwayland-client` links the queue, proxy, and display helpers; `-lrt` links `shm_open` on older toolchains where POSIX shared memory still lives outside libc. On current glibc the flag is harmless.

Run the result inside a compositor session (`./checker` under Weston, Sway, Mutter, or `weston --socket=test` with `WAYLAND_DISPLAY` pointed at it). A gray checkerboard window titled "Checker" should appear and group under `org.example.checker` in the dock.

## 9.9 Reading the exchange under `WAYLAND_DEBUG`

Prefix the client with `WAYLAND_DEBUG=1` to dump every message:

```sh
WAYLAND_DEBUG=1 ./checker 2>&1 | head -n 60
```

Requests show as `->`, events as `<-`, each tagged with the object ID and interface:

```
[1234567.890]  -> wl_display@1.get_registry(new id wl_registry@2)
[1234567.891]  -> wl_registry@2.bind(..., "xdg_wm_base", 1, new id xdg_wm_base@5)
[1234567.892]  -> xdg_wm_base@5.get_xdg_surface(new id xdg_surface@7, wl_surface@6)
[1234567.893]  -> xdg_surface@7.get_toplevel(new id xdg_toplevel@8)
[1234567.894]  -> xdg_toplevel@8.set_title("Checker")
[1234567.895]  -> xdg_toplevel@8.set_app_id("org.example.checker")
[1234567.896]  -> wl_surface@6.commit()
[1234567.910]  <- xdg_wm_base@5.ping(serial=42)
[1234567.911]  -> xdg_wm_base@5.pong(serial=42)
[1234567.920]  <- xdg_toplevel@8.configure(width=640, height=480, states=[])
[1234567.921]  <- xdg_surface@7.configure(serial=7)
[1234567.922]  -> xdg_surface@7.ack_configure(serial=7)
[1234567.923]  -> wl_shm@4.create_pool(new id wl_shm_pool@9, fd=5, size=1228800)
[1234567.924]  -> wl_surface@6.attach(wl_buffer@10, 0, 0)
[1234567.925]  -> wl_surface@6.damage(0, 0, 640, 480)
[1234567.926]  -> wl_surface@6.commit()
```

Read it as a checklist. Title and app ID must precede the empty commit. `ping` must be followed promptly by `pong` with the same serial. If several `xdg_surface.configure` events arrive before the client responds, ack only the last serial: skipping the intermediates is legal, but acking a serial twice, acking an older serial after a newer one, or acking a serial never sent raises `invalid_serial`. The ack should precede the first buffer attach for that serial. A zero-size toplevel configure means "client picks"; anything else should match the next buffer dimensions. `wl_buffer.release` arriving much later than the next draw is normal; rewriting the same mapping before it arrives is the tearing bug from Chapter 8. If the log ends with `xdg_toplevel@8.close`, the main loop should exit and the following lines should be proxy destroys, not more draws.

## Recap

- Window policy lives outside core Wayland in `stable/xdg-shell.xml`, a frozen stable protocol. Core gives blank surfaces; xdg-shell gives them window meaning.
- `xdg_wm_base` (bind version 1) is the factory and liveness channel. Answer every `ping` with `pong` and the same serial.
- `get_xdg_surface` assigns the one window role to a `wl_surface`; `get_toplevel` makes it a top-level window. A second role on the same surface is a fatal protocol error.
- Sizing is a handshake: compositor sends `configure(serial)`, client replies `ack_configure(serial)` then draws. `set_window_geometry` restricts tiling and input to the clickable rectangle when shadows or rounded corners pad the buffer.
- `set_title` labels the titlebar and switcher; `set_app_id` (for example `org.example.checker`) groups dock icons. Toplevel `configure(width, height)` with non-zero values should be honored; `close` means exit the dispatch loop.
- Zero to window is connect, bind `shm` / `compositor` / `wm_base`, create surface, wrap plus label, empty commit, configure-ack-draw-commit, dispatch until closed. Never draw before the first configure.
- Generate code with `wayland-scanner client-header` and `private-code`, compile with `cc ... -lwayland-client -lrt`, and verify the message order with `WAYLAND_DEBUG=1`.

## Exercises

1. **Recolor the board.** Change the two checker colors, rebuild, and run under `WAYLAND_DEBUG=1`. Confirm the `attach` / `damage` / `commit` triple still follows `ack_configure`, and explain in one paragraph why `damage` covering the full window is correct here but wasteful for a cursor blink.
2. **Honor the configured size.** Resize the window in a tiling compositor and log the `xdg_toplevel.configure` widths and heights. The sample already stores them; extend `draw_frame` to print the size it draws at, verify buffers match non-zero configures, and describe what the user would see if you ignored configure and always drew 640x480. On a floating-only compositor the exercise still runs, but expect width and height 0x0 (no opinion, client picks) rather than a forced size.
