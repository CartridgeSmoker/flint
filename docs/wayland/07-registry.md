# Chapter 7: Globals and the Registry

> **Objectives.** By the end of this chapter you will be able to:
> - Explain the registry: the id-to-interface mapping and the id 1 bootstrap.
> - Run the `get_registry` flow: request, listen, roundtrip, bind.
> - Decode a `global` event: numeric name, interface string, version.
> - Bind a global with `wl_registry_bind` and string comparison.
> - Advertise a global with `wl_global_create` and serve per-client binds.

A fresh connection is almost empty. The client owns one object: `wl_display` at id 1. Every other id is assigned per connection at runtime, so the client cannot guess them. The registry is the directory that maps ids to interfaces: the client asks `wl_display` for a registry, the server replies with one `global` event per singleton, and the client binds the ones it needs.

```
 client                            server
   |                                  |
   |--- wl_display.get_registry ------>|
   |<-- wl_registry.global (each) -----|
   |--- wl_registry.bind ------------->|
   |<-- initial events (per bind) -----|
```

That is the chapter in four arrows. The rest is what each arrow carries.

## 1. Why a registry at all

Every Wayland object is addressed by a numeric id, but the numbers are assigned per connection at runtime. Id 1 is the single exception: it is always `wl_display`, created implicitly by `wl_display_connect`. Every other id is negotiated. Without a directory, the client would have no way to learn that id 7 means `wl_shm` at version 1 on this compositor.

Globals are the compositor's singletons: `wl_compositor`, `wl_shm`, one `wl_output` per monitor, `wl_seat`, `xdg_wm_base`. Each has an interface string, a supported version, and a numeric name valid only within one registry stream.

Binding creates a per-client object with a fresh id at a negotiated version. Two rules follow: match on interface strings, never on numeric names; and roundtrip before binding, because `global` events arrive asynchronously.

## 2. The `get_registry` flow in four steps

Step 1: request a registry. Call `wl_display_get_registry` ONCE per connection and keep it; each call creates a new registry object that leaks server resources until disconnect. (The XML describes `get_registry` as creating a registry object the client uses to list and bind the globals available from the compositor):

```c
struct wl_registry *registry =
    wl_display_get_registry(display);
```

Step 2: attach a listener before anything can arrive. The registry has two events: `global` and `global_remove` (when a monitor is unplugged, for example):

```c
static void registry_global(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    printf("%-20s version %u name %u\n", interface, version, name);
}

static void registry_global_remove(void *data,
    struct wl_registry *registry, uint32_t name)
{
    printf("removed global name %u\n", name);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

wl_registry_add_listener(registry, &registry_listener, NULL);
```

The listener must be installed immediately, before the roundtrip, or the burst can arrive with nobody home.

Step 3: roundtrip. This flushes `get_registry` and blocks until the server's burst of `global` events arrives:

```c
if (wl_display_roundtrip(display) < 0) {
    /* Server hung up: disconnected. */
}
```

Toolkits sometimes use a second roundtrip to flush callbacks registered during the first. For a lister, one is enough.

Step 4: bind by string comparison, at the advertised version or a lower one the client implements:

```c
if (strcmp(interface, "wl_compositor") == 0) {
    struct wl_compositor *compositor = wl_registry_bind(registry, name,
        &wl_compositor_interface, 4);
}
```

`wl_registry_bind` takes the event's numeric name, the generated `*_interface` descriptor, and the version. Binding the same global twice yields two independent objects.

The message sequence for a typical startup looks like this:

```
C -> S  wl_display@1.get_registry  -> new id wl_registry@2
S -> C  wl_registry@2.global(1, "wl_compositor", 6)
S -> C  wl_registry@2.global(2, "wl_shm", 1)
S -> C  wl_registry@2.global(3, "wl_output", 4)
C -> S  wl_registry@2.bind(2, "wl_shm", 1) -> new id wl_shm@4
S -> C  wl_shm@4.format(WL_SHM_FORMAT_XRGB8888)
```

Wire note: the XML `bind` takes only numeric name + new id; the generated `wl_registry_bind` wrapper injects the interface string and version onto the wire, which is why the trace shows `(2, "wl_shm", 1)` for what the XML calls `bind(name, id)`.

Some globals emit initial state right after bind (shm formats, output geometry, seat capabilities). A client that uses the object without dispatching first reads stale state.

## 3. What a `global` event means

Each event carries three fields:

- **`name` (uint32).** Handle within this registry. Pass it back to `bind`; remember it to recognise `global_remove`. Not a wire object id, not portable.
- **`interface` (string).** Protocol name, e.g. `"wl_output"`. This is what the client matches on.
- **`version` (uint32).** Highest version the server supports. The client binds at `min(server version, client-implemented version)`. Versions shown in this chapter are illustrative; always compare interface strings, never versions or numeric names.
- **Binding above the server-advertised version** is a server-side protocol error: the server tears down the connection with a `wl_display.error`.
- **Binding above what the client code implements** is a client bug: the server may then send events the client's listener does not handle.

Negotiation is per bind, not per connection: one client may bind `wl_compositor` at 4 and `wl_output` at 2 from the same registry.

> **Misconception: "global names are stable across connects, so I can cache name 3 means shm."** No. Names are assigned per registry in list order. One run advertises `wl_shm` as 2, the next as 5; a second monitor adds another `wl_output` row. Compare the interface string on every connect and keep the name only for that registry's lifetime.

`global_remove` carries only the name. Drop cached state for it. Already-bound objects stay valid and their requests are ignored until the client destroys the proxy; this absorbs the race where the server withdraws a global the client just bound. Binding a removed name is a protocol error that kills the connection: `wl_registry_bind` returns a proxy with no return-code contract, so the failure surfaces as a `wl_display` error or disconnect, not a NULL return.

## 4. Complete program: globals lister

This program is the standard "is my compositor reachable" smoke test:

```c
#include <stdint.h>
#include <stdio.h>
#include <wayland-client.h>

static void registry_global(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    printf("global %u: %s version %u\n", name, interface, version);
}

static void registry_global_remove(void *data,
    struct wl_registry *registry, uint32_t name)
{
    printf("global %u: removed\n", name);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to Wayland display\n");
        return 1;
    }

    struct wl_registry *registry =
        wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    /* Flush get_registry and wait for the burst of globals. */
    if (wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "disconnected while listing globals\n");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 1;
    }

    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
```

Build and run:

```sh
cc lister.c -o lister $(pkg-config --cflags --libs wayland-client)
./lister
```

Expected output under a running compositor resembles:

```
global 1: wl_compositor version 6
global 2: wl_shm version 1
global 3: wl_output version 4
global 4: wl_seat version 9
global 5: xdg_wm_base version 6
```

Empty output with exit 0 means socket trouble, not an empty compositor: `wl_display_connect(NULL)` falls back to `$WAYLAND_DISPLAY` (default `wayland-0`) under `$XDG_RUNTIME_DIR`, and the roundtrip returns with no events. Check:

```sh
echo "RUNTIME=${XDG_RUNTIME_DIR} DISPLAY=${WAYLAND_DISPLAY:-wayland-0}"
ls -l "$XDG_RUNTIME_DIR/${WAYLAND_DISPLAY:-wayland-0}"
```

If the socket is missing, start a compositor first. A stale file from a crashed compositor connects and then hangs up.

Tip: `WAYLAND_DEBUG=1 ./lister` dumps every message sent and received: `get_registry`, the `global` burst, `registry_destroy`. The shortest complete protocol trace available; from here on, generated wrappers and debug logs carry the same information readably.

## 5. Server side: registering globals and serving binds

At startup the compositor advertises one `wl_global` per singleton with `wl_global_create`: interface, highest implemented version, owner pointer, bind handler:

```c
#include <wayland-server.h>
/* Generated by wayland-scanner from wayland.xml: wl_output_interface,
 * wl_output_send_geometry/send_mode/send_scale/send_done, and the
 * WL_OUTPUT_* enums come from here, NOT from wayland-server.h. */
#include "wayland-server-protocol.h"

struct output {
    struct wl_global *global;
    int width, height;
    /* ... mode, scale, compositor back-pointer ... */
};

static void bind_output(struct wl_client *client, void *data,
    uint32_t version, uint32_t id);

static int advertise_output(struct output *o, struct wl_display *display)
{
    o->global = wl_global_create(display,
        &wl_output_interface, 4, o, bind_output);
    return o->global ? 0 : -1;
}
```

On bind, libwayland calls the handler with the client, the owner pointer, the requested version, and the client-chosen id. The handler creates a per-client resource clamped to the bound version, attaches the request table and user data, and pushes initial state:

```c
static void output_release(struct wl_client *client,
    struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct wl_output_interface output_impl = {
    .release = output_release,
};
/* Name-collision warning: `struct wl_output_interface` is the request
 * table YOU implement (release, ...), while `wl_output_interface` (no
 * `struct`) is the generated global metadata constant (`struct
 * wl_interface`) passed to wl_global_create/wl_resource_create. Same
 * spelling, different namespaces. */
/* Note: `release` is since=3. Setting the slot is harmless: clients
 * bound at v1/v2 can never send it, so the handler simply never runs. */

static void output_resource_destroy(struct wl_resource *resource)
{
    /* TODO: unlink this resource from the output's tracked resource
     * list once one exists. This example keeps no list, so there is
     * nothing to unlink yet. This callback also runs on client
     * disconnect without release, so cleanup here must not assume
     * release ran first. */
    (void)resource;
}

static void bind_output(struct wl_client *client, void *data,
    uint32_t version, uint32_t id)
{
    struct output *o = data;

    /* Never serve a newer interface than both sides implement. */
    if (version > (uint32_t)wl_output_interface.version)
        version = (uint32_t)wl_output_interface.version;

    struct wl_resource *resource = wl_resource_create(client,
        &wl_output_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &output_impl,
        o, output_resource_destroy);

    /* Push current state so the client never sees a blank output.
     * Geometry alone is not enough: the protocol requires
     * geometry/mode followed by done, so always send the mode too. */
    wl_output_send_geometry(resource, 0, 0,
        o->width, o->height, WL_OUTPUT_SUBPIXEL_UNKNOWN,
        "Acme", "Panel", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT,
        o->width, o->height, 60000);
    if (version >= 2) {
        wl_output_send_scale(resource, 1);
        wl_output_send_done(resource);
    }
}
```

Four details matter. The resource is per client per bind, each carrying the version that client asked for. The clamp guards a client requesting newer than the server. `release` is `wl_resource_destroy`, and the destroy callback is where a tracked resource list would be unlinked so later broadcasts skip it (this example keeps no list yet, hence the TODO; the callback also runs on disconnect without release). Initial events must respect the bound version: `wl_output_send_done` and `wl_output_send_scale` exist only from version 2, so guard them.

Use `wl_global_remove` to withdraw a global: it stops future binds and emits `global_remove`, while existing resources survive until released. Use `wl_global_destroy` to free the `wl_global` itself (typically at compositor shutdown).

## Recap

The registry is the bootstrap directory: from id 1 (`wl_display`), the client requests a registry, collects the `global` burst with a roundtrip, and binds wanted interfaces by string at a negotiated version. Names are per-registry handles, never cached. Servers advertise with `wl_global_create`, serve each bind with a version-clamped per-client resource, destroy on `release`, and guard initial events by bound version.

## Exercises

1. **Run the lister.** Build and run it against your compositor and record the globals and versions. Disable a monitor or restart with different outputs and run it again: which rows change, which names shift, which strings stay put?
2. **Trace with `WAYLAND_DEBUG`.** Run `WAYLAND_DEBUG=1 ./lister` and label each line as request or event and mark the new-id allocations. Extend the lister to bind `wl_shm`, dispatch once more, and identify the `format` events sent in reply.
