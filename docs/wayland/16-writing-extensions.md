# Chapter 16: Writing Your Own Wayland Protocol Extension

> **Objectives:** By the end of this chapter you will be able to decide when a custom extension is justified, read and write protocol XML correctly, explain how opcodes are assigned and why order matters, generate both ends with `wayland-scanner`, apply the append-only compatibility oath, and run the 5-point design checklist on any new interface.

You know enough Wayland to invent a word. Resist that urge a little longer. A new extension is a permanent burden on everyone who adopts it. This chapter shows when that cost is worth it, how the machinery works, and how to avoid regret.

## When to Extend, and When Not To

Extend for exactly three reasons: a **compositor-specific effect** (blur, rounded corners, panel position), a **device or policy decision** (kiosk lockdown, input injection, color management), or an **internal tool** (debug overlay, test harness, private shell channel). In all three cases the compositor is the authority and both ends are under your control.

For everything else, prefer an existing protocol. Need app identity? That is `xdg-activation`, not `my_app_id`. Search `wayland-protocols` first. If something is close, add an event or enum value to it. Keep scope tiny: one manager, one object, two or three requests. You can always add later. You can never take back what you shipped.

## XML Anatomy

XML is the single source of truth; code and docs are generated from it (see [Chapter 04](04-interfaces-xml-patterns.md) for the full pattern catalogue). `<protocol>` holds interfaces. `<interface name version>` defines an object type. `<request>` is client-to-server, `<event>` is server-to-client, `<enum>` holds `<entry>` constants. Requests and events carry `<arg>` children: `int`/`uint` (32-bit), `fixed` (coordinates), `string` (UTF-8), `object` (existing object, `interface="wl_surface"`), `new_id` (creates one, `interface="my_blur"`), `array` (byte blob), `fd` (descriptor, out-of-band). Add `allow-null="true"` for nullable objects or strings; without it codegen assumes non-null. Give every interface, request, event, and entry a `summary="..."` and a `<description>` where needed. The scanner turns summaries into doc comments.

## Opcodes Are Document Order

Requests and events are numbered from zero in **document order**, each direction separately. The first `<request>` is opcode 0, the second is 1. Names never travel on the wire, only these integers. So reorder and rename both break compatibility: reordering remaps integers to handlers so old clients invoke the wrong method, and renaming renames every generated function so all downstream code stops compiling. Treat shipped XML as stone. Append new elements at the end. Never reorder, remove, or rename.

## Scanner Codegen: Never Hand-Pack

Never pack messages by hand. `wayland-scanner` generates both ends: client stubs per request plus a listener struct for events, and a server dispatch table plus a `*_send_*` helper per event (see [Chapter 05](05-libwayland.md) for what libwayland does with the generated code).

```sh
wayland-scanner client-header my-blur.xml my-blur-client.h
wayland-scanner server-header my-blur.xml my-blur-server.h
wayland-scanner private-code  my-blur.xml my-blur-protocol.c
```

Link against `libwayland-client` or `libwayland-server`. The scanner guarantees opcode agreement, marshalling, and version checks. Hand-packing invites drift that only explodes at runtime. If you write a raw opcode number, stop and regenerate.

## The Append-Only Oath

After stabilization: **only add, never change.** You may add requests, events, enum values, and whole interfaces. Never remove, reorder, change an argument type, or reuse an opcode. Every addition raises the interface `version` and carries `since="N"` matching the version that introduced it (v1 elements may omit it, implying `since="1"`; e.g. `<request name="set_sigma" since="2">` in `<interface version="2">`); a client binds at `v <= N` and the compositor must check `wl_resource_get_version(resource)` and never send a v2 event to a v1 client. Advertise with `wl_global_create` and cap the bound version (see [Chapter 07](07-registry.md) for version negotiation). The `unstable` prefix grants exactly one break: while unstable you may rename and reset versioning with a flag day where everyone migrates. Once you drop the prefix, append-only forever. Plan that rename early, migrate once, then keep the oath.

## The 5-Point Design Checklist

Run every interface through these five questions before writing compositor code.

### 1. Atomic or Immediate? Prefer Staged State Plus a Commit

If the client changes several properties that must take effect together, stage them with setters and apply them with one commit request. Our blur stages `radius` with `set_radius` and applies it with `commit`, so radius and region never tear. Use immediate fire-and-forget requests only for true transients like `poke` or `trigger_screenshot`. If two setters in a row could show a half-applied frame, you need a commit.

### 2. Who Authorizes Power? Require a Serial for Privileged Actions

Anything stronger than decoration needs proof of user intent. The pattern is serial authentication: the compositor issues a serial with each input event or configure, and the privileged request echoes a recent one back; stale or forged values get rejected: define an `invalid_serial` (or similar) error in your XML `<enum name="error">` and raise it with `wl_resource_post_error`. Activation, screenshots, and input emulation all work this way. Convenience without authentication becomes a sandbox escape.

### 3. Single or Double-Buffered? Double-Buffer on the Surface Commit

For anything visual, keep pending and current copies and copy pending to current inside your commit hook, piggybacking on the `wl_surface.commit` the client already sends each frame. This keeps radius, clip, and contents in lockstep with no extra round trip. Apply immediately only for non-visual flags. If pixels move, double-buffer.

### 4. What Happens on Destroy? Handshake Late Events: Error or No-Op

Decide what happens when the client destroys the object while events are in flight, and document it. Raise a `wl_display` protocol error to kill clients that misuse a dead object, or silently no-op events that race teardown such as a fading tooltip. Never leave it unspecified. Always mark the destructor `type="destructor"` and test the race: destroy, flush a pending event, assert the outcome. With `type="destructor"` codegen gives `*_destroy` proxy semantics: the client must send nothing further on the object after destroy, while the server must tolerate in-flight events that race teardown.

### 5. Who Proposes Mirrored State? Use Configure Plus Ack

When both sides hold a copy of some state, the compositor sends a `configure` proposal with a serial and the client answers `ack_configure(serial)`; only the last acked serial counts, and newer configures supersede older ones. This is how `xdg-shell` keeps resizes sane. Copy it whenever the compositor constrains the client, such as clamping a blur radius to hardware limits.

## Minimal Worked Example: `my_blur`

One manager, one object, three requests, one event:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<protocol name="my_blur">
  <interface name="my_blur_manager" version="1" summary="create blur objects">
    <request name="get_blur" summary="create blur for a surface">
      <arg name="id" type="new_id" interface="my_blur" summary="new object"/>
      <arg name="surface" type="object" interface="wl_surface" summary="target"/>
    </request>
    <request name="destroy" type="destructor" summary="destroy manager"/>
  </interface>
  <interface name="my_blur" version="1" summary="blur behind one surface">
    <request name="set_radius" summary="stage radius">
      <arg name="radius" type="uint" summary="pixels, 0 disables"/>
    </request>
    <request name="commit" summary="apply staged radius"/>
    <request name="destroy" type="destructor" summary="remove blur"/>
    <event name="applied" summary="radius took effect">
      <arg name="radius" type="uint" summary="active radius"/>
    </event>
  </interface>
</protocol>
```

Client, using generated stubs only:

```c
struct my_blur *blur = my_blur_manager_get_blur(manager, surface);
my_blur_set_radius(blur, 12);
my_blur_commit(blur);
wl_surface_commit(surface); /* effect lands with this frame */
```

Server, double-buffered with clamped echo (`applied` defers to the surface-commit hook):

```c
static void blur_set_radius(struct wl_client *c,
    struct wl_resource *r, uint32_t radius) {
    struct blur *b = wl_resource_get_user_data(r);
    b->pending_radius = radius > MAX_RADIUS ? MAX_RADIUS : radius;
}
static void blur_commit(struct wl_client *c, struct wl_resource *r) {
    struct blur *b = wl_resource_get_user_data(r);
    b->dirty = true; /* copied to current on wl_surface.commit below */
}
static void blur_on_surface_commit(struct wl_listener *listener, void *data) {
    struct blur *b = wl_container_of(listener, struct blur, surface_commit);
    if (!b->dirty)
        return;
    b->current_radius = b->pending_radius;
    b->dirty = false;
    my_blur_send_applied(b->resource, b->current_radius);
}
```

Staged setter plus commit (1), no serial because decoration is not power (2), surface-commit piggyback (3), explicit destructor (4), `applied` echoing the clamped value as a baby configure (5).

> **Misconception: "Renaming is safe if the wire format is identical."**
> No. The wire carries opcodes but the ecosystem carries names. Renaming `set_radius` to `set_sigma` renames every generated function and listener field, forcing all clients and compositors to edit and recompile, and it tempts a reorder that silently remaps opcodes. Fix names once during `unstable` with a flag day. After `stable`, live with the name and add a sibling instead.

## Recap and Exercises

Extend only for compositor effects, policy, or internal tools; otherwise reuse a standard protocol. XML is the truth, opcodes come from document order, and the scanner generates both ends. After stabilization, only add, gated on the bound version. The checklist (atomic commit, serial auth, double-buffering, destroy handshake, configure plus ack) is your ten-minute review.

1. **Write a toy XML and generate.** Design `my_clock` with `get_clock`, `set_format` plus `commit`, and a `tick` event. Run the three scanner commands above; confirm the client header has `my_clock_manager_get_clock` and the server header has `my_clock_send_tick`. Then reorder two requests, regenerate, and watch the opcodes swap in the private code.
2. **Run the 5-point review on your own design.** Take any extension idea and write one paragraph per checklist point: atomic or immediate, what serial guards it, where it double-buffers, what destroy does, whether it needs configure plus ack. If any paragraph is "not sure," the design is not ready.
