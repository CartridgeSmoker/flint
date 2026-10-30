# Learning Wayland: A Practical Book

A cohesive guide to the Wayland protocol for client and compositor
developers, covering concepts, mechanics, and working code throughout.

Contents:

1. Why Wayland: a short history
2. How a Wayland desktop fits together
3. The protocol in layers
4. libwayland: the reference implementation
5. Displays, event loops, registries
6. Pixels on screen: surfaces and buffers
7. Your first window: XDG shell basics
8. Surfaces, perfected: lifecycle, timing, regions
9. Hands on keys and pointers: seats and input
10. The rest of XDG shell
11. Copy, paste, drag and drop
12. The extension ecosystem, and writing your own
Appendix: debugging the wire

---

## 1. Why Wayland: a short history

To understand Wayland, start in a computer lab in the 1980s.

In 1984 at MIT, Project Athena wanted cheap graphical terminals
driven by big shared machines. Bob Scheifler and colleagues invented
the X Window System for exactly that split: the X client (your
program) could run on a VAX down the hall, while the X server ran on
your desk, drawing windows and reading your keyboard.

That split gave X its famous feature: network transparency. Every
button, keystroke and rectangle was a small message you could send
over the wire. Finalized as X11 in 1987, it was brilliant for its
time. Workstations were weak, there was no GPU to speak of, networks
were small and trusted, fonts lived on the server because shipping
pixels was too expensive. The server drew everything; clients asked
politely: draw a line here, put text there.

The world changed and X11 did not. By the late 1990s your PC dwarfed
Athena's VAX. You had 3D hardware, video, translucency, hotplugged
monitors. Core X11 could do none of it, so we bolted it on: XRandR
for resizing, RENDER and COMPOSITE and DAMAGE for anti-aliasing and
shadows, DRI for direct 3D that bypassed the server entirely, XInput
and XInput2 for everything beyond a two-button mouse.

The result worked, but every frame was imperfect. In X any client
could draw to its piece of the screen at any time. The compositor, if
you ran one, was an afterthought: a regular X client intercepting
drawings through COMPOSITE, adding shadows, pushing them back. Nothing
guaranteed drawing finished before the monitor scanned it out, so you
got the horizontal rip through every movie. Everyone had a
workaround; nobody had a fix.

Security was worse. The X server trusted everyone equally. Any client
could screenshot the whole desktop, including your banking tab, or
listen to every keystroke even when unfocused. Keyloggers on X are
not an exploit; they are the API. Focus stealing, global grabs, one
frozen app freezing the session: one all-powerful server, no
isolation.

Kristian Hogsberg lived that pain. In 2008, working at Red Hat on DRI2
and kernel modesetting (moving display-mode control out of the X
server into the kernel where it belonged), he asked what was left for
X to do. Clients already rendered themselves with Cairo, Qt and
OpenGL. The kernel already set modes and scheduled buffers. All that
remained was a protocol for "here is a finished buffer" and "here is
your input", with a compositor assembling the screen tear-free.

He called it Wayland and built Weston as its reference compositor:
tiny, strict, proof the idea worked. For years it stayed an
experiment; GNOME and KDE were busy keeping X alive for millions.

The turn came from tinkerers. Around 2015 Drew DeVault and others
wanted an i3-like tiling manager on Wayland and found they would have
to reinvent DRM handling, input, layout and buffer management from
scratch. So they wrote wlroots: shared plumbing so small compositors
could exist. Sway, the i3 clone on wlroots, gave hackers a daily
Wayland desktop years before the big desktops were smooth.

Then defaults flipped. Fedora made Wayland default in stages, Ubuntu
followed with 22.04. The last holdout was NVIDIA, pushing its own
buffer API instead of the GBM API everyone else used; when NVIDIA
adopted GBM in 2021 the dam broke. In 2024 explicit sync landed,
letting drivers agree exactly when a buffer is ready and killing a
class of flicker. HDR and color management are in active protocol work.

The philosophical shift is simple. In X, the server draws. In
Wayland, clients draw themselves. Your app renders its window to an
offscreen buffer with the GPU, exactly as it wants, then hands that
finished buffer to the compositor. The compositor (Mutter, KWin, Sway,
Hyprland) arranges buffers, draws decorations, and asks the kernel to
show one complete frame timed to vsync. The kernel modesets. Nobody
draws to the visible screen mid-scanout. No tearing by design.

If X was a central office that did everything for you, Wayland is a
good landlord. You decorate your own flat; the landlord keeps the
building coherent and stops anyone walking into your flat uninvited.
That isolation is why we did all this work.

## 2. How a Wayland desktop fits together

Read your machine bottom-up.

Hardware: your GPU renders (shaders, video, pixel buffers) and scans
out. Sixty or 144 times a second the display controller reads a
finished buffer down the cable line by line. Swap mid-scan and you
tear; everything above exists to prevent that. Input devices emit raw
HID reports: contact down here, key with this raw code pressed.

Kernel: Linux does the heavy lifting. For display, DRM/KMS (Direct
Rendering Manager / Kernel Mode Setting) appears as nodes in
`/dev/dri/`. `/dev/dri/card0` is the primary node: modes, resolutions,
final commits, privileged only. `/dev/dri/renderD128` is the render
node: any app may render offscreen, none may touch the screen. That
split founds Wayland security: rendering open to all, modesetting
reserved. For input, every keyboard and mouse is a `/dev/input/event*`
evdev node emitting type/code/value streams. Only privileged code
reads them directly.

Userspace, the alphabet soup, each sparing you from writing a driver:

- libdrm: thin wrapper over kernel display ioctls for compositors.
- Mesa: OpenGL/Vulkan drivers plus GBM (Generic Buffer Management),
  how a client allocates a GPU buffer and shares its handle without
  copying pixels.
- libinput: on top of evdev, turning raw events into two-finger
  scroll, tap-to-click, palm rejection, acceleration.
- udev: plug/unplug notifications for monitors and mice.
- xkbcommon: raw keycodes to symbols for your layout (keycode 30 to
  `a` or `A` or `q` on AZERTY), modifiers, latches, groups.
- pixman: software rasterizer fallback.
- libwayland: the wire protocol: words and file descriptors over a
  Unix socket.
- Toolkits GTK, Qt, SDL: what you normally program against. You open
  a GTK window; the toolkit does the Wayland handshake quietly.
- wlroots: compositor toolkit (Sway and many small compositors)
  handling KMS, libinput and buffers. Mutter and KWin do the same
  in-house.

```
 +--------------------------------------------------+
 | Clients (GTK / Qt / SDL apps)                    |
 |  "I render my own window into private buffers"   |
 +------------------------+-------------------------+
                          | Wayland protocol
                          | (/run/user/1000/wayland-0)
 +------------------------v-------------------------+
 | Compositor (Mutter / KWin / Sway / Hyprland)      |
 |  composites + routes input + KMS commit           |
 +---+----------+-----------+-----------+-----------+
     |          |           |           |
  libinput  xkbcommon     Mesa/GBM   libdrm
     |          |           |           |
 +---v----------v-----------v-----------v-----------+
 | Kernel: evdev + DRM/KMS (card0 priv, render shared)|
 +------------------------+-------------------------+
                          |
 | Hardware: GPU render + scanout, USB HID devices   |
 +--------------------------------------------------+
```

Two words carry the meaning. A client is any app showing something;
it renders privately, owns only its buffers, sees neither screen nor
other clients. The compositor is the one privileged program talking
to the kernel for everyone. It composites (one finished buffer per
window into one frame), displays (atomic KMS commit on vsync), and
dispatches input.

Input dispatch is a conscientious receptionist. The compositor alone
reads evdev; libinput normalizes, xkbcommon interprets, then the
compositor hit-tests pointer position, tracks keyboard focus, and
sends narrowly scoped events (`wl_pointer.enter`, `wl_keyboard.key`)
over each client's private socket, only to the client that should see
them. There is no global key query, no window enumeration, no shared
pot. A background window never receives another window's keys. That
is why Wayland clients cannot keylog: isolation is structural.

Mental model: your app paints a picture in private; the compositor
hangs all pictures in a gallery and guards the door; the kernel owns
the building.

## 3. The protocol in layers

Wayland is three layers plus agreed patterns. Wire format moves bytes;
objects give bytes meaning; XML lets machines generate both sides.

The wire is a stream of 32-bit words in host byte order. Same machine,
no endian dance. Types: `int`/`uint` (32-bit), `fixed` (signed 24.8
fixed point for subpixel coordinates), `object` (id), `new_id` (id
with creation semantics), `enum` (a uint with a name), `string` (uint
length including NUL, bytes, zero pad to word boundary), `array` (uint
length, bytes, pad), `fd` (out of band via `SCM_RIGHTS` ancillary
data, the reason Wayland needs Unix sockets: the bytes move in-band,
the buffers behind them move as descriptors).

Each message: sender object id, then one word split into 16-bit size
(including the 8-byte header) and 16-bit opcode, then arguments in
interface order. Both sides know opcode 2 on `wl_surface` is `damage`
with four ints because they read the same XML. Example layout:

```
wl_surface@6.damage(0, 0, 800, 600)
word 0: object id ............ 6
word 1: size = 24, opcode = 2
word 2..5: x, y, width, height
```

No tags, no field names, no version on the wire. Shared expectation
is the protocol.

Objects are the model. You send requests to objects; you receive
events from objects. Id 0 is null. Id 1 is always `wl_display`. Both
sides allocate: clients from 1 to 0xFEFFFFFF, servers from
0xFF000000 to 0xFFFFFFFF, so a `new_id` argument never collides.

Before any of that, connect. `WAYLAND_SOCKET` set means a connected
fd was handed to you; use it. Else `WAYLAND_DISPLAY`: absolute path
if it contains `/`, else joined to `$XDG_RUNTIME_DIR`. Else
`$XDG_RUNTIME_DIR/wayland-0`. That fallback is why the first
compositor answers on `wayland-0`, the next on `wayland-1`.

Requests go client to server; events go server to client; neither
blocks. Opcodes are numbered from zero in XML document order,
requests and events independently. `damage` is a request you send
("these pixels changed"); `enter` is an event you receive ("your
surface now overlaps this output, here is the `wl_output`"). React,
do not poll.

The XML is the single source of truth. Trimmed `wl_surface`:

```xml
<interface name="wl_surface" version="6">
  <request name="destroy" type="destructor"/>
  <request name="attach">
    <arg name="buffer" type="object" interface="wl_buffer"
      allow-null="true"/>
    <arg name="x" type="int"/>
    <arg name="y" type="int"/>
  </request>
  <request name="damage">
    <arg name="x" type="int"/>
    <arg name="y" type="int"/>
    <arg name="width" type="int"/>
    <arg name="height" type="int"/>
  </request>
  <event name="enter">
    <arg name="output" type="object" interface="wl_output"/>
  </event>
</interface>
```

`wayland-scanner` turns this into headers, stubs and dispatch tables.
Never reorder casually: appending is safe, inserting renumbers.

Three patterns keep you sane. Atomicity: almost nothing on
`wl_surface` applies immediately; `attach`, `damage`, regions,
transforms and scales stage into pending state and `commit` applies
them together, so imperfect states never show. Every frame can be
perfect. Lifetimes: destruction is a handshake, not a hammer. Each
sender's messages arrive in order, but client requests and server
events can cross; tolerate the stray event for a just-destroyed id,
mark destructors `type="destructor"`, never reuse an id until the
other side knows. Versioning: bind a version at registry time, speak
that dialect; only append requests, events and enum members, never
remove or reorder; the one sanctioned break is unstable-to-stable
graduation.

## 4. libwayland: the reference implementation

"Wayland" is the protocol; libwayland is the reference runtime plus
code generator. Toolkits hide it, but it is underneath every native
client and compositor.

The package gives you `libwayland-client`, `libwayland-server`,
`wayland-scanner`, and `/usr/share/wayland/wayland.xml` defining
`wl_display`, `wl_registry`, `wl_compositor`, `wl_surface`,
`wl_output` and friends.

Open `wayland-util.h` first. `struct wl_list` is the kernel-style
intrusive linked list (embed `struct wl_list link`, `wl_list_init`,
`wl_list_for_each`); compositors list everything with it. `struct
wl_array` is a grow-only buffer (`wl_array_add`,
`wl_array_release`) for variable payloads. Fixed-point helpers
(`wl_fixed_from_int`, `wl_fixed_to_double`) bridge protocol
coordinates to C doubles. Log handlers (`wl_log_set_handler_client`,
`_server`) route fatal errors to your logger.

Never hand-write marshalling. Describe in XML, generate:

```sh
wayland-scanner client-header wayland.xml wayland-client-protocol.h
wayland-scanner server-header wayland.xml wayland-server-protocol.h
wayland-scanner private-code  wayland.xml wayland-protocol.c
```

Include `<wayland-client.h>` in app code, not the generated header
directly; compile the generated C alongside and link
`-lwayland-client` (or `-lwayland-server`).

Proxies versus resources is the central split. On the client every
remote object is a `wl_proxy`. Generated helpers like
`wl_surface_attach(surface, buffer, 0, 0)` call `wl_proxy_marshal`
underneath and queue for `wl_display_flush`. You rarely touch
`wl_proxy_*` directly. On the server every per-client object is a
`wl_resource`, always owned by exactly one `wl_client`. Incoming
requests dispatch to your implementation; outgoing events go through
generated `wl_output_send_geometry(resource, ...)`; misbehaving
clients die cleanly via `wl_resource_post_error`, delivering
`wl_display.error` without harming others.

Interfaces and listeners tie it together. Each XML interface becomes
a descriptor, a listener struct, and glue:

```c
static void surface_enter(void *data, struct wl_surface *s,
    struct wl_output *o) { /* appeared on an output */ }
static void surface_leave(void *data, struct wl_surface *s,
    struct wl_output *o) { }

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};
wl_surface_add_listener(surface, &surface_listener, my_context);
```

The `void *data` is your lifeline: your window struct handed back to
every callback. Requests are plain functions; the server is the
mirror (implements requests, sends events, registers with
`wl_resource_set_implementation`). Client listens and requests;
server implements and notifies. Once that reversal clicks, every
generated header reads the same.

## 5. Displays, event loops, registries

`wl_display` is the ignition key: the only object existing before
negotiation, owning socket, ids, queue and errors. In XML it is tiny
(`sync`, `get_registry`; `error`, `delete_id`) and load-bearing:
`sync` barriers roundtrips, `get_registry` opens discovery, `error`
reports protocol death.

Client connect is one call:

```c
#include <wayland-client.h>
int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1; /* no compositor */
    wl_display_disconnect(display);
    return 0;
}
```

`NULL` reads `WAYLAND_DISPLAY` and falls back to `wayland-0`.
`wl_display_connect_to_fd` covers inherited sockets;
`wl_display_get_fd` exposes the fd for `poll`.
`cc connect.c -o connect -lwayland-client`.

Server mirrors it: create, name a socket, publish globals, run:

```c
#include <wayland-server.h>
int main(void) {
    struct wl_display *display = wl_display_create();
    const char *socket = wl_display_add_socket_auto(display);
    if (!socket) return 1;
    wl_display_run(display);
    wl_display_destroy(display);
    return 0;
}
```

`add_socket_auto` skips taken names (`wayland-1`, ...);
`add_socket(display, "wayland-1")` pins one for tests
(`WAYLAND_DISPLAY=wayland-1 ./connect`). `run` blocks until
`wl_display_terminate`.

Event loops diverge usefully. Servers own a `wl_event_loop`
(`wl_display_get_event_loop`): `add_fd`, `add_timer`, `add_signal`,
`add_idle`; `wl_display_run` dispatches forever and flushes clients.
With your own main loop, poll `wl_event_loop_get_fd` and on readiness
call `wl_event_loop_dispatch` plus `wl_display_flush_clients`.
Clients have no loop object: `wl_display_flush` sends,
`wl_display_dispatch` blocks and invokes listeners,
`wl_display_roundtrip` flushes plus `sync`-barriers until the server
answers. Setup uses roundtrip; steady state uses flush/poll/dispatch:

```c
int fd = wl_display_get_fd(display);
struct pollfd pfd = { .fd = fd, .events = POLLIN };
while (running) {
    wl_display_flush(display);
    poll(&pfd, 1, -1);
    if (pfd.revents & POLLIN)
        wl_display_dispatch(display);
}
```

Discovery is the registry. Fresh connections know only the display.
`wl_display_get_registry` plus a `wl_registry_listener`
(`global(name, interface, version)`, `global_remove(name)`) plus one
`wl_display_roundtrip` delivers the burst; inside `global`,
string-compare and `wl_registry_bind` what you need. Complete
globals lister to keep:

```c
static void global(void *d, struct wl_registry *r, uint32_t name,
    const char *iface, uint32_t ver) {
    printf("global %u: %s version %u\n", name, iface, ver);
}
static void global_remove(void *d, struct wl_registry *r,
    uint32_t name) { printf("removed %u\n", name); }
/* main: connect, get_registry, add_listener, roundtrip,
   destroy, disconnect */
```

Empty output means socket trouble, not code trouble: check
`WAYLAND_DISPLAY` and `$XDG_RUNTIME_DIR`. And learn the one debug
trick now: `WAYLAND_DEBUG=1 ./your-client` dumps every message both
ways. "Black window" becomes "never sent commit" in seconds.

Servers publish with `wl_global_create(display, &wl_output_interface,
3, data, bind_output)`; on bind, `wl_resource_create(client,
&wl_output_interface, version, id)`, clamp to the bound version,
`wl_resource_set_implementation` with a request table (notably
`release` destroying the resource), then emit mandatory initial state
such as `wl_output_send_geometry` (and `send_done` for version 2+).
Each client gets its own resource at its own version; pass per-output
data through the `data` argument.

Connect, registry, roundtrip, bind, dispatch. Create, socket,
globals, run. Every later protocol rides the same path.

## 6. Pixels on screen: surfaces and buffers

Every pixel on a Wayland desktop is a deal between a place
(`wl_surface`: rectangular endpoint, top-left origin, surface-local
coordinates, initially blank, sizeless, meaningless) and pixels
(`wl_buffer`: deliberately opaque store, shm or GPU or vendor-only;
compositor promises to try showing it).

Surfaces come from the `wl_compositor` global (bind version 4):

```c
if (strcmp(interface, wl_compositor_interface.name) == 0)
    compositor = wl_registry_bind(registry, name,
        &wl_compositor_interface, 4);
/* later */
struct wl_surface *surface =
    wl_compositor_create_surface(compositor);
```

New surfaces do nothing until given a buffer and a role, then
committed.

Simplest pixels are `wl_shm`: a file both sides map. Only
`ARGB8888`/`XRGB8888` are guaranteed; listen for `format` events for
the rest; prefer XRGB8888 without alpha. Recipe: random-named POSIX
shm, unlink immediately, size, map, hand over. Sizing is arithmetic:
1920x1080 XRGB8888 is 8294400 bytes per frame, 16588800
double-buffered; stride is `width*4`; `offset + stride*height` must
stay inside the pool.

```c
static int allocate_shm_file(size_t size) {
    /* shm_open(random "/wl_shm-XXXXXX", O_RDWR|O_CREAT|O_EXCL, 0600),
       shm_unlink, ftruncate with EINTR retry; see chapter 7
       for the full 30-line helper */
}
int stride = width * 4, size = stride * height;
int fd = allocate_shm_file(size);
uint32_t *px = mmap(NULL, size, PROT_READ|PROT_WRITE,
    MAP_SHARED, fd, 0);
/* draw: px[y*width+x] = 0xFFRRGGBB */
struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
    width, height, stride, WL_SHM_FORMAT_XRGB8888);
wl_surface_attach(surface, buffer, 0, 0);
wl_surface_damage(surface, 0, 0, width, height);
wl_surface_commit(surface);
```

`attach` selects the next buffer, `damage` marks what changed,
`commit` goes live atomically. Null buffer plus commit hides. Same
buffer without damage redraws nothing.

Servers enable shm with `wl_display_init_shm` plus
`wl_display_add_shm_format`, then on commit test
`wl_shm_buffer_get(resource)` and bracket reads with
`begin_access`/`end_access`, using `get_data/stride/format/width/
height`. The client owns the file; the pointer is borrowed.

shm wastes two copies on real GPUs (GPU render, CPU readback, GPU
re-upload). Zero-copy is dmabuf: share the GPU buffer by handle. Most
apps take the EGL shortcut: `eglGetPlatformDisplayEXT` with
`EGL_PLATFORM_WAYLAND_KHR`, config with `EGL_WINDOW_BIT`,
`wl_egl_window_create`, `eglCreatePlatformWindowSurfaceEXT`,
`eglMakeCurrent`, draw, `eglSwapBuffers`, `wl_egl_window_resize` on
resize. Mesa turns the swap into `linux-dmabuf` attach; compositor
authors study wlroots import. Learn shm first because every step is
visible; take EGL when speed matters.

Roles give surfaces meaning. Bare surfaces cannot be told apart:
window, cursor, drag icon, wallpaper need different semantics (cursors
do not minimize; windows do not stick to mice). Assign exactly one
role once, by creating `xdg_surface`, `set_cursor` and friends for
that `wl_surface`. A second role is a protocol error. That strictness
prevents "move window or move pointer?" ambiguity.

## 7. Your first window: XDG shell basics

Core has no windows; policy lives in extensions. XDG shell (in
wayland-protocols, `stable/xdg-shell`) turns surfaces into windows.
One global, `xdg_wm_base`. Bind it, wrap each `wl_surface` with
`get_xdg_surface`, then take a role: `xdg_toplevel` (normal window) or
`xdg_popup` (menu). Answer `ping` with `pong` carrying the same serial
or be killed as hung.

The heart is configure/acknowledge: compositor proposes (toplevel
size/states, then `xdg_surface.configure(serial)`), you dispose and
`ack_configure(serial)`, then commit a fitting buffer. No ack, no new
size; ack without commit leaves the compositor waiting.
`set_window_geometry` trims client-side shadows and rounded corners
from the clickable, alignable rectangle. `set_title`/`set_app_id`
(label for titlebars, class like `org.example.checker` for docks)
belong before the first commit.

Zero-to-window, every client repeats it:

1. Connect, get registry.
2. Bind `wl_shm`, `wl_compositor`, `xdg_wm_base`.
3. Create `wl_surface`.
4. Wrap with `get_xdg_surface`.
5. `get_toplevel`, title, app id.
6. Commit empty to request first configure.
7. On configure: ack, draw sized shm buffer, attach, commit.
8. Dispatch, answering pings.

Complete 640x480 checkerboard (fresh pool per frame for clarity;
buffer release destroys after compositor release). Generate glue,
then build:

```sh
wayland-scanner client-header \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
  xdg-shell-client-protocol.h
wayland-scanner private-code \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
  xdg-shell-protocol.c
cc -o checker checker.c xdg-shell-protocol.c -lwayland-client -lrt
```

```c
struct client_state {
    struct wl_display *display;
    struct wl_shm *shm;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    int width, height; bool closed;
};
/* shm helpers randname/create_shm_file/allocate_shm_file
   as in chapter 6 */

static void buffer_release(void *d, struct wl_buffer *b) {
    wl_buffer_destroy(b);
}
static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void draw_frame(struct client_state *s) {
    int stride = s->width * 4, size = stride * s->height;
    int fd = allocate_shm_file(size);
    uint32_t *px = mmap(NULL, size, PROT_READ|PROT_WRITE,
        MAP_SHARED, fd, 0);
    for (int y = 0; y < s->height; ++y)
        for (int x = 0; x < s->width; ++x)
            px[y*s->width+x] =
                (((x/32)+(y/32))%2==0) ? 0xFF404040 : 0xFFC0C0C0;
    struct wl_shm_pool *pool =
        wl_shm_create_pool(s->shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0,
        s->width, s->height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(buf, &buffer_listener, NULL);
    wl_surface_attach(s->surface, buf, 0, 0);
    wl_surface_damage(s->surface, 0, 0, s->width, s->height);
    wl_surface_commit(s->surface);
    wl_shm_pool_destroy(pool); close(fd); munmap(px, size);
}

static void xdg_surface_configure(void *d,
    struct xdg_surface *xs, uint32_t serial) {
    struct client_state *s = d;
    xdg_surface_ack_configure(xs, serial);
    draw_frame(s);
}
static void toplevel_configure(void *d, struct xdg_toplevel *t,
    int32_t w, int32_t h, struct wl_array *st) {
    struct client_state *s = d;
    if (w > 0 && h > 0) { s->width = w; s->height = h; }
}
static void toplevel_close(void *d, struct xdg_toplevel *t) {
    ((struct client_state*)d)->closed = true;
}
static void wm_base_ping(void *d, struct xdg_wm_base *w,
    uint32_t serial) { xdg_wm_base_pong(w, serial); }
/* listeners + registry binds for shm/compositor/wm_base,
   main: connect, roundtrip, check globals, add listeners,
   create surface/xdg_surface/toplevel, title/app_id,
   commit empty, dispatch until closed, destroy, disconnect.
   Full 200-line version follows this exact skeleton. */
```

The empty commit says "I exist, configure me"; each configure draws.
Release destroys buffers so none leak or reuse busy. Pool dies right
after creation; the buffer holds its own reference.
`WAYLAND_DEBUG=1 ./checker` shows binds, `get_xdg_surface`,
`set_title`, commit, configure/ack, attach/damage, pings.

## 8. Surfaces, perfected: lifecycle, timing, regions

Simple attach/commit shows pixels; this chapter makes them correct,
efficient, shaped and sharp.

Pending means later, commit means now. `attach`, damage, opaque/input
regions, transforms, scales and role state all stage into pending;
`commit` applies atomically. Resend overwrites pending (attach B after
A forgets A). The first commit with a buffer maps the surface
visible. Habit: one `redraw()` doing attach, damage, commit together.

Do not render blind; ask for a frame. Drawing faster than 60 or 144
Hz wastes power; drawing minimized or occluded windows wastes all of
it. `wl_surface_frame` returns a `wl_callback`; later
`done(time_ms)` is permission to draw one frame. Destroy the callback
in the handler, request the next immediately, advance animation by
time deltas (identical speed on any refresh, correct jump after
throttle), then draw/attach/damage/commit. One outstanding request;
forgetting to re-request stalls quietly. Kick off once after the
first commit.

```c
static void frame_done(void *d, struct wl_callback *cb,
    uint32_t time) {
    struct app *a = d;
    wl_callback_destroy(cb);
    uint32_t el = (a->last && time > a->last) ?
        time - a->last : 0;
    a->last = time;
    a->offset = (a->offset + (int)(el*24/1000)) % 32;
    struct wl_callback *nx = wl_surface_frame(a->surface);
    wl_callback_add_listener(nx, &frame_listener, a);
    draw_checker(a);
    wl_surface_attach(a->surface, a->buffer, 0, 0);
    wl_surface_damage_buffer(a->surface, 0, 0,
        INT32_MAX, INT32_MAX);
    wl_surface_commit(a->surface);
}
```

Damage is what changed. Prefer `damage_buffer` (buffer coordinates,
ignores transform/scale; matches how you draw) over `damage`
(surface coordinates, transform-aware). Partial damage wins:
blinking caret or clock damages a tiny rectangle; scrolling or resize
damages all via the `INT32_MAX` idiom (clipped to size). Damage is
double-buffered; attach without damage shows stale content, the
classic blank-window bug.

Regions are rectangle sets for hints. `create_region`, `add`,
`subtract` (512 square minus 256 hole for a cutout), then
`set_opaque_region` (blend-skip optimization; default empty assumes
translucent) or `set_input_region` (click-through shadows, round
dials; default is whole surface). Null clears (opaque to transparent
assumption, input to whole surface). Double-buffered to commit;
destroy the region right after set; the compositor copies.

Subsurfaces are core's only role: children via
`wl_subcompositor.get_subsurface(surface, parent)`, forming trees,
positioned by `set_position`, stacked by `place_above`/`place_below`.
Video plus subtitles, decorations on GL content, plane-friendly
embedded layers. `set_sync`/`set_desync` controls buffer timing
(position/stack always ride the parent commit; sync child buffers
wait for parent for atomicity, desync apply on child commit for
latency). One role per surface; restructure by destroy/rebuild.

HiDPI is explicit. `wl_output.scale` (v2+) plus `wl_surface.enter/
leave` (which outputs contain you): track the maximum entered scale,
render Nx (`1024x1024` buffer for `512x512` logical at 2x),
`wl_surface_set_buffer_scale` (v3+) plus commit. Compositor downscales
cleanly to 1x monitors, upscales if you under-provide (blurry).
Logical size drives layout and input; buffer size drives sharpness;
forgetting the label doubles apparent size, forgetting pixels blurs.

Pending/commit, frame timing, damage, regions, subsurfaces, scale:
six tools, finished surfaces.

## 9. Hands on keys and pointers: seats and input

Input starts with where the user sits: a `wl_seat`, one keyboard plus
one pointer plus one touch per person. Most sessions expose one
(`seat0`/`default`). Capabilities (`pointer=1, keyboard=2, touch=4`)
arrive via `capabilities`/`name` (bind version 7); hotplug is normal
(dock, Bluetooth), so create `get_pointer`/`get_keyboard`/`get_touch`
on appearance and `release` (not destroy, the quirk) on disappearance.

Serials are permission slips. Clicks, keys and touches carry a serial
proving recency; privileged asks (popup grab, move/resize, drag, copy)
must quote one. Stale or invented serials are ignored. Save the serial
from the causing event (button press for menus and drags, key for
keyboard menus and copies); never cache forever.

Frames turn packets into events. Motion, button and axis (or multiple
touch contacts) may arrive as separate messages for one logical
action; `wl_pointer.frame` / `wl_touch.frame` ends the batch.
Accumulate into a struct with a mask, dispatch once on frame, clear.
Keyboards need no frame; each key stands alone after the keymap.

```c
enum pointer_bits { P_ENTER=1, P_LEAVE=2, P_MOTION=4,
    P_BUTTON=8, P_AXIS=16 };
struct pointer_accum { uint32_t mask; double x, y;
    uint32_t button, state; double ax, ay; };
/* handlers set bits + fields; frame dispatches + clears */
```

Pointers: `enter(serial, surface, x, y)` (fixed point via
`wl_fixed_to_double`; save the serial) and `leave`. Cursors are
client-side: `set_cursor(serial_from_enter, surface_with_image,
hotspot_x, hotspot_y)`; null hides (video, paint). `wayland-cursor`
loads themes (`wl_cursor_theme_load`,
`wl_cursor_image_get_buffer`; attach, damage, commit, set). Set on
focus/image change, not per motion. `motion(time, x, y)`; `button
(serial, time, code, pressed/released)` with Linux `BTN_LEFT/RIGHT/
MIDDLE` (0x110/111/112) and up; `axis(time, vertical/horizontal,
value)` plus `axis_source(wheel/finger/continuous/tilt)`,
`axis_stop` (gesture end) and `axis_discrete` (notch count for wheel
emulation). Accumulate smooth values per frame; flings stay smooth.

Keyboards need XKB: scancode (physical key, Wayland `key`) is not a
letter; keysym (current meaning) plus modifiers/locks/groups becomes
Unicode. Four objects: context, keymap, state. `keymap(format, fd,
size)` hands a file (only `xkb_v1` defined): mmap, `new_from_string
(TEXT_V1)`, `xkb_state_new`, replace old (layouts change at runtime).
Keys: evdev code plus 8 (the historical offset) into
`get_one_sym` (shortcuts) and `get_utf8` (text). `modifiers
(depressed, latched, locked, group)` feeds straight to
`xkb_state_update_mask`. `enter(serial, surface, keys_array)` (clear
stuck keys via `wl_array_for_each`, save serial) and `leave`.
`repeat_info(rate, delay)` means repeat is client-side: one press,
your timer synthesizes until release.

```c
xkb_keycode_t code = key + 8;
xkb_keysym_t sym = xkb_state_key_get_one_sym(k->state, code);
xkb_state_key_get_utf8(k->state, code, buf, sizeof buf);
```

Touch speaks contacts: integer `id` valid `down` to `up`, reusable
after. `down(serial, time, surface, id, x, y)` begins, `motion(time,
id, x, y)` moves, `up(serial, time, id)` ends, `cancel` aborts all
(system gesture stole them; reset, expect no `up`s), `frame` groups,
`shape(major/minor)` + `orientation` (v6+, ellipse in surface units)
vary brushes. Fixed point in, double before use; gestures update on
frame.

Discipline over cleverness: hotplug create/release, serial from cause,
accumulate to frame, XKB plus 8 with mask feedback, touch ids as
short leases honoring cancel. Solid input users never notice.

## 10. The rest of XDG shell

Happy path shows a window; this chapter makes it behave. Nothing is a
command; everything is a proposal with a serial.

Configure/ack for real: `xdg_toplevel.configure(width, height,
states)` then `xdg_surface.configure(serial)`; reply
`ack_configure(serial)` before committing the matching buffer. Zero
size means compositor defers ("keep yours, or natural if none"),
never make a zero buffer, converge to non-zero promptly. States:
`maximized`/`fullscreen` (fill edge to edge, no floating shadows),
`resizing` (fast, slightly ugly allowed), `activated` (bright, typing;
dim otherwise), `tiled_left/right/top/bottom` (no shadows/rounding on
glued edges), `suspended` (occluded; stop rendering, save battery).
Ack latest without immediate commit to batch; never commit new state
under an old serial. `close` is a request: save quickly, quit; honor
the second close unconditionally (`close_requested` flag; users press
twice).

```c
static void on_toplevel_configure(void *d, struct xdg_toplevel *t,
    int32_t w, int32_t h, struct wl_array *s) {
    /* store pending_w/h (0 = keep), parse states */
}
static void on_surface_configure(void *d, struct xdg_surface *s,
    uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    schedule_redraw(app); /* layout, then commit */
}
```

Client to compositor is asking: `set_min/max_size` early (a
12-pixel terminal is broken, not minimal), `set_maximized/
unset_maximized`, `set_fullscreen(output_or_null)/unset_fullscreen`,
`set_minimized` (no unset; only the user returns you). Request, wait
configure, ack, commit; optimistic draw races and loses.

Popups (menus, tooltips, dropdowns) are full stacks per popup
(`wl_surface` + `xdg_surface` + `xdg_popup` via
`get_popup(parent_xdg, positioner)`); chains nest (`Doc` child of
`Recent` child of `File`) for stacking and dismissal. Configure gives
`x/y/width/height` relative to parent geometry, possibly slid
onscreen: honor what you got. Key-navigated menus need
`grab(seat, cause_serial)`; stale serials open ungrabbed (draw anyway,
do not freeze). `popup_done` dismisses; server keeps structs briefly
so races do not explode: on done destroy if still owned and clear;
on local pick destroy, clear, ignore late done. Destroy top-down;
killing a parent with live children is a protocol error. Tear down
eagerly, never cache.

You cannot position yourself (thirty years of splash-screen abuse
ended that); three polite verbs replace it: `move(seat, serial)` on
header drag, `resize(seat, serial, edges)` with
`top/bottom/left/top_left/...` from your hit-test (compositor tracks,
sends `resizing` configures), `show_window_menu(seat, serial, x, y)`
for titlebar right-click (compositor draws minimize/move/close). With
client decorations these are mandatory, else painted shut. Who draws
frames is `zxdg_decoration_manager.get_toplevel_decoration` plus
`set_mode(client/server)/unset_mode` (humble default; let compositor
pick) answered by `configure(mode)` as the decision. Hardcoding
client-side earns double titlebars on GNOME.

Positioners are one-shot intent: fill, hand to `get_popup`, destroy.
`set_size` required; `set_anchor_rect`, `set_anchor`, `set_gravity`,
`set_constraint_adjustment(slide/flip/resize)` say where you want to
hang (menu button, hovered word) and what onscreen-keeping is
allowed. Compositor decides; tooltips slide, bottom combos flip up.
Allow flipping or draw half offscreen, brokenly.

Ack promptly, honor given size, children before parents, never assume
requested position. Native everywhere, the highest Wayland compliment.

## 11. Copy, paste, drag and drop

No bytes cross the wire; only promises plus pipes. One global,
`wl_data_device_manager`: `get_data_device(seat)` (selection and DnD
per keyboard-mouse pair) and `create_data_source` (your outgoing
offers). Version 3 adds DnD action negotiation; bind high, code
defensively.

Three objects. `wl_data_source` (yours when copying/dragging out):
`offer(mime)` per type (faithful first, `text/plain;charset=utf-8`
always as fallback), `set_actions(copy/move/ask)` for drags; events
`target(mime)` (destination pick), `send(mime, fd)` (write now to
pipe), `cancelled`, plus `dnd_drop_performed`/`dnd_finished`/`action`.
`wl_data_offer` (theirs when pasting/receiving; never created by
you): events `offer(mime)`, `source_actions`/`action`; requests
`accept(serial, mime_or_null)` (NULL rejects; always call, sources
optimize on `target`), `receive(mime, fd)` (pipe to read),
`finish()` (drop consumed; enables move-delete), `set_actions(mask,
preferred)` (vote copy/move). `wl_data_device` (referee per seat):
events `data_offer(id)`, `enter(serial, surface, x, y, id)`, `motion`,
`drop`, `leave` (always comes; "drag over, reset highlights"),
`selection(id_or_null)`; requests `set_selection(source, serial)`,
`start_drag(source, origin, icon_or_null, serial)`. Descriptors are
transport: pipe write end to `receive`, read to EOF, close both;
leaked fds exhaust long sessions.

Copy: inside Ctrl+C/click handler, save the serial (focus-stealing
prevention: only recently interacted apps may claim; timer-thread
copies meet silence, correctly).

```c
struct wl_data_source *src =
    wl_data_device_manager_create_data_source(manager);
wl_data_source_offer(src, "text/plain;charset=utf-8");
wl_data_source_offer(src, "text/html");
wl_data_device_set_selection(device, src, cause_serial);
```

No text sent; "ask me on paste". New copier cancels you
(`cancelled`; free buffer). Paste triggers your `send`; write exactly
that MIME, close. `set_selection(NULL, serial)` clears (still needs a
valid serial). Managers snapshot by pasting each new offer then
becoming source themselves.

Paste: you stored the offer from `selection()`. On Ctrl+V, `accept
(paste_serial, best_mime)`, `receive(mime, write_end_of_pipe)`,
close write end, read read end to EOF, parse as that MIME. Without
accept some compositors never send. Offers die on next
`selection(NULL)`/`leave`: destroy handle, clear pointer. Drops add
`finish()` after consuming; Escape/reject omits it (source keeps
original).

Drag: thresholded press creates source, offers, actions, optional
icon surface (dedicated DnD-icon role; never reuse toplevel/cursor
surfaces), `start_drag(source, origin, icon_or_null, press_serial)`.
No press serial, no drag. Destination: `enter` (offer + serial;
`accept` promptly so source sees `target`), `motion` (accept again),
`set_actions(can, preferred)`, on `drop` receive/read then `finish()`
if consumed, on `leave` reset. Source: `action(chosen)` tracks votes,
`dnd_drop_performed` ("destination will receive"), `dnd_finished`
(over, success or not; move-sources delete originals only now, never
on drop). Many events because each cheat was tried: serials stop
theft, accept stops conversions, finish/cancelled stops move loss,
pipes stop compositor RAM bloat.

Nail plain-text copy/paste with serials and fd hygiene (90 percent of
use), then NULL-icon copy-only drag, then icons, move, multi-MIME.
Incremental correctness beats ambitious halves.

## 12. The extension ecosystem, and writing your own

Core is grammar; extensions are vocabulary. Panels, vsync feedback,
HiDPI, sharing, shortcuts all live outside `wayland.xml`.
`wayland-protocols` ships `stable` (done, rely), `staging` (agreed
problem, mostly solution, small revisions), `unstable` (lab notebook,
exact-version gating; xdg-shell was once `xdg_shell_unstable_v6`).
Mechanically identical binds (`wl_registry_bind` at min(yours,
theirs); lower is safe since only additions ever ship). Treat staging
as dependable, unstable as try-with-fallback; all-stable purity costs
HiDPI, decorations, activation.

Must-knows by pain avoided. presentation-time: nanosecond stamp,
period, sequence, vsync/flip/discard flags over coarse `frame` ms;
schedule on frame, adapt on feedback (video, latency). viewporter
plus fractional-scale-v1 plus xdg-output: logical size/scale, crop/
destination, true 150% buffers; integer `wl_output.scale` alone blurs
modern laptops. single-pixel-buffer: solid colors without arrays
(placeholders, letterbox). pointer-constraints (`lock` for FPS
cameras, `confine` for panning) plus relative-pointer (raw
unaccelerated deltas) for shooters and brushes; without them cameras
spin at edges and strokes inherit desktop curves. idle-inhibit while
playing/presenting, destroyed on pause (backpack-awake laptops are
leaked inhibitors). layer-shell: panels, docks, backgrounds, lockers,
keyboards anchored to edges above/below windows; desktop chrome's
first dependency, normal apps keep out. xdg-decoration (who draws
frames) and xdg-activation (focus tokens from user actions for
launchers and "open in editor"; no more steal-or-miss). primary-
selection (select-to-copy, middle-click paste; mirrors data-device)
versus privileged data-control/clipboard-manager (managers and remote
desktop, allowlisted; support primary, do not depend on control).
linux-dmabuf-v1 (GPU buffers by reference; zero-copy video/GL) plus
explicit-sync (per-buffer fences; tear-free under load; EGL/Vulkan
mandatory). session-lock and presets for lockers and color tools. A
player wants presentation, inhibit, dmabuf, activation; a game wants
lock, deltas, dmabuf; a panel wants layer-shell and outputs. Pick by
symptom.

Writing your own starts as XML: protocol, interfaces, requests
(client to server), events (server to client), enums. Args: `int`,
`uint`, `fixed`, `string`, `object`, `new_id`, `array`, `fd`, plus
`allow-null`. Opcodes are document order; reorder/rename breaks.
Scanner generates both ends; hand-packed opcodes mean you left the
path.

```xml
<protocol name="my_effects">
  <interface name="my_blur_manager_v1" version="1">
    <request name="get_blur">
      <arg name="id" type="new_id" interface="my_blur_v1"/>
      <arg name="surface" type="object" interface="wl_surface"/>
    </request>
    <request name="destroy" type="destructor"/>
  </interface>
</protocol>
```

One rule: append only. New requests, events, enum values,
interfaces; never remove, reorder or reinterpret; gate on bound
version, never send v2 events to v1 binds. Unstable-to-stable buys
one rename/reset/migrate break; then append forever.

Checklist before shipping: atomic commit (stage plus commit, no
visible transients)? Serial auth for power (focus, clipboard, lock,
screenshot) with validation? Double-buffered state with
surface-commit piggybacking? Destroy handshake (who first, late
events, error versus no-op)? Configure/ack for mirrored state
(proposal, serial, ack)? Five yeses feel Wayland-native; each no is a
race you will rediscover one by one.

## Appendix: debugging the wire

`WAYLAND_DEBUG=1` dumps every message both ways: interface, opcode,
args, fds, serials. Verbose and perfect: reproduce minimally, read
around failure. Missing ack, wrong destroy order, stale selection
serial all confess in the log. `weston-info` (or compositor
equivalent) lists globals and versions: confirm exposure before
blaming binds. Installed `wayland.xml` is ground truth when docs and
behavior disagree.

Bind humbly, degrade gracefully, ask instead of command,
acknowledge instead of assume, clean up pipes, popups and
inhibitors. The silent partner behind every frame will make you look
good.
