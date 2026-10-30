# Chapter 5: libwayland - The Reference Implementation

> **Objectives.** By the end of this chapter you will be able to:
> - Name what libwayland ships and where `wayland.xml` lives.
> - Use `wayland-util.h` (`wl_list`, `wl_array`, `wl_fixed`, log handlers).
> - Run `wayland-scanner` to generate protocol glue at build time.
> - Distinguish client proxies (`wl_proxy`) from server resources (`wl_resource`).
> - Wire generated interfaces and listeners on both sides, including the `void *data` lifeline.

Most Wayland code never calls libwayland directly. It calls code libwayland generated. The layering is fixed: XML describes the protocol, the scanner turns XML into C, `libwayland-client` and `libwayland-server` turn that C into wire traffic, and your code sits on top.

```
 wayland.xml + your-protocol.xml
        |
        v
 wayland-scanner (build time)
        |
        +---> *-client-protocol.h + *-protocol.c ---> libwayland-client
        +---> *-server-protocol.h + *-protocol.c ---> libwayland-server
        |
        v
 proxies (wl_proxy) / resources (wl_resource)
        |
        v
 your client code / your compositor code
```

## 1. What the package contains

Distributions split libwayland into three logical parts:

1. **`libwayland-client.so`** - connection setup, event queue, proxy marshalling. Toolkits and raw clients link this.
2. **`libwayland-server.so`** - client tracking, resource lifetime, event-loop helpers.
3. **`wayland-scanner`** plus **`wayland.xml`** - a build-time code generator and the core protocol description. The scanner emits C; it does no runtime work.

Two paths to memorise:

- Core protocol: `/usr/share/wayland/wayland.xml`, installed with libwayland.
- Extensions: `/usr/share/wayland-protocols/stable/...` and `.../unstable/...`, installed with the separate `wayland-protocols` package (for example `xdg-shell`).

If the first path is missing, ask `pkg-config` where the data directory moved:

```sh
pkg-config --variable=pkgdatadir wayland-scanner
ls /usr/share/wayland/wayland.xml
wayland-scanner --help
```

Headers follow the same split: `wayland-client.h`, `wayland-server.h`, `wayland-util.h`. Core protocol glue ships pre-generated: `/usr/include/wayland-client-protocol.h` and `/usr/include/wayland-server-protocol.h` are installed with libwayland, and the matching compiled glue already lives in `libwayland-client.so` / `libwayland-server.so`. Extension protocols are the opposite: headers such as `xdg-shell-client-protocol.h` are never installed; you generate them from the `wayland-protocols` XML at build time and compile and link the glue yourself.

Most apps link one side only. Compile and link with:

```sh
pkg-config --cflags --libs wayland-client
pkg-config --cflags --libs wayland-server
```

## 2. `wayland-util.h`: lists, arrays, fixed point, logging

`wayland-util.h` is shared by client and server. Its header comments are excellent; read them.

**`wl_list`: intrusive doubly-linked list.** Each element embeds `struct wl_list link;`. Initialise with `wl_list_init`, insert with `wl_list_insert`, remove with `wl_list_remove`. Iterate with `wl_list_for_each`, and with `wl_list_for_each_safe` when the body may remove the current element. Recover the outer struct with `wl_container_of`. Compositors use `wl_list` for clients, resources, seats, and outputs.

**`wl_array`: grow-only buffer.** It holds `size`, `alloc`, and `void *data`. Initialise with `wl_array_init`, grow it with `wl_array_add` or `wl_array_copy`, iterate with `wl_array_for_each`, release backing store with `wl_array_release`. There is no remove-from-middle. It appears in input and SHM-pool handling.

**`wl_fixed_t`: 24.8 fixed point.** The wire has no floats, so coordinates travel as 24 bits integer plus 8 bits fraction. Convert at the boundary, never by casting:

```c
#include <wayland-util.h>

wl_fixed_t f = wl_fixed_from_int(10);
int i = wl_fixed_to_int(f);
wl_fixed_t g = wl_fixed_from_double(1.5);
double d = wl_fixed_to_double(g);
```

**Log handlers.** `wl_log_set_handler_client` and `wl_log_set_handler_server` replace the default stderr logger.

**Marshalling structs: `struct wl_interface`, `struct wl_message`, `union wl_argument`.** Generated code is tables of these structures for marshalling/unmarshalling: each `wl_interface` names an interface and points at its `wl_message` request/event arrays, each `wl_message` records a name, signature string, and types array, and each `wl_argument` carries one decoded value in a union. You rarely touch them directly, but read them when a generated wrapper misbehaves. For the low-level `wl_proxy_marshal*`, queue, and dispatch functions behind the wrappers, read `wayland-client-core.h` next.

## 3. `wayland-scanner`: XML in, C out

You never hand-write `wl_surface_attach`. You generate it. The scanner has five modes:

- `client-header` - client wrappers, types, listener structs.
- `server-header` - server interface structs and `send_*` event helpers.
- `enum-header` - enum-only header without wrappers or tables.
- `private-code` - shared marshalling tables both sides link; prefer this over `public-code`.
- `public-code` - older public marshalling variant; prefer `private-code`.

Manual use looks like this (stdin/stdout redirection form; the file-arg form also works on current scanners):

```sh
wayland-scanner client-header < /usr/share/wayland/wayland.xml > wayland-client-protocol.h
wayland-scanner server-header < /usr/share/wayland/wayland.xml > wayland-server-protocol.h
wayland-scanner private-code < /usr/share/wayland/wayland.xml > wayland-protocol.c
```

Real projects run this at build time so generated code matches the pinned XML. With Meson:

```meson
scanner = find_program('wayland-scanner')
protos = dependency('wayland-protocols')
xdg_xml = protos.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_client = custom_target('xdg-shell-client',
  input: xdg_xml, output: 'xdg-shell-client-protocol.h',
  command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
xdg_glue = custom_target('xdg-shell-glue',
  input: xdg_xml, output: 'xdg-shell-protocol.c',
  command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
```

Compile and link the `private-code` glue output with your client; the header alone is not enough.

CMake does the same via `add_custom_command`. Commit the XML version, not the generated C. Generated files live in the build directory.

Include order matters: always include `wayland-client.h` (or `wayland-server.h`) before the generated protocol header. The generated header assumes base types such as `struct wl_surface` and `wl_fixed_t` already exist.

> **Misconception: "just include the generated header directly."** No. The generated header is not self-contained and is not installed system-wide. Include the base library header first, then the generated header from your build directory. If your editor cannot resolve it, fix the build include path, not the `#include` line.

## 4. Proxies on the client, resources on the server

Client and server see the same protocol from opposite ends.

**Client: `wl_proxy`.** Every client object (`wl_surface`, `wl_display`, and so on) wraps an opaque `struct wl_proxy`. Sending a request serialises arguments in opcode order; receiving an event dispatches to your listener. The generated `wl_surface_attach()` is a thin wrapper around `wl_proxy_marshal_flags`. You should rarely touch `wl_proxy` directly. A cast to `wl_proxy` usually means you missed a generated wrapper.

**Server: `wl_resource` and `wl_client`.** The compositor holds one `struct wl_resource *` per object per client. Three clients with one surface each mean three resources, each bound to its own `struct wl_client *` connection. Destroying a client destroys its resources and nobody else's. On protocol violation the compositor calls `wl_resource_post_error(resource, code, "...")`, which tears down that client and delivers `wl_display.error`.

Lifetime mirrors the split. The client sends a destructor request. The server calls `wl_resource_destroy`, optionally notifying the client. Neither side frees the peer's memory.

## 5. Interfaces and listeners: a round trip

Generated code converges on two patterns: listener structs on the client, interface structs on the server.

On the client, each interface with events gets a listener struct with one callback per event, in XML order. For `wl_surface`, `enter` and `leave` fire when an output starts or stops covering the surface:

```c
#include <wayland-client.h>
#include "wayland-client-protocol.h"

static void surface_enter(void *data,
    struct wl_surface *s, struct wl_output *o)
{
    ((struct app_state *)data)->visible_outputs++;
}

static void surface_leave(void *data,
    struct wl_surface *s, struct wl_output *o)
{
    ((struct app_state *)data)->visible_outputs--;
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

wl_surface_add_listener(surface, &surface_listener, app);
```

The trailing `void *data` in `add_listener` is your lifeline: libwayland passes it unchanged to every callback, so point it at the owner struct. Leave no entry NULL unless you intend to drop that event.

The server mirrors this. It implements a request table, binds it with `wl_resource_set_implementation`, and emits events with `send_*` helpers:

```c
static void surface_attach(struct wl_client *client,
    struct wl_resource *resource, struct wl_resource *buffer,
    int32_t x, int32_t y)
{
    struct my_surface *s = wl_resource_get_user_data(resource);
    s->pending_buffer = buffer;
    s->pending_x = x;
    s->pending_y = y;
}

static const struct wl_surface_interface surface_impl = {
    .attach = surface_attach,
    /* .damage, .commit, .destroy, ... */
};

wl_resource_set_implementation(resource, &surface_impl, my_surface, destroy_cb);
wl_surface_send_enter(resource, output_resource);
```

`wl_resource_set_implementation` binds the request table, the user-data pointer, and the destructor. `wl_surface_send_enter` is the counterpart of the client's `enter` callback. On dispatch, libwayland looks up object ID -> interface -> decodes -> finds listeners -> invokes with args. Object references such as `buffer` and `output_resource` come from display/registry handling, covered in the next chapters.

## Recap

libwayland is two libraries plus a generator: `libwayland-client` speaks through proxies, `libwayland-server` tracks per-client resources, and `wayland-scanner` produces the typed glue from XML at build time. `wayland-util.h` supplies lists, arrays, fixed-point conversion, and log hooks. Clients register listeners with an owning `void *data`; servers implement request tables, bind them with `wl_resource_set_implementation`, emit with `send_*`, and punish violations with `post_error`.

## Exercises

1. **Run the scanner by hand.** Generate `client-header`, `server-header`, and `private-code` from `/usr/share/wayland/wayland.xml` into a scratch directory. Find `struct wl_surface_listener`, check `enter`/`leave` against the example above, then find `wl_surface_attach` and note how it forwards to marshalling. Delete the scratch files; real builds regenerate them.
2. **Read `wayland-util.h` end to end.** For each of `wl_list`, `wl_array`, `wl_fixed`, and the log-handler typedefs, write one sentence on when you would use it. Note the `_safe` iteration macro and `wl_container_of`, then find one use of each in a client or compositor tree you have checked out.
