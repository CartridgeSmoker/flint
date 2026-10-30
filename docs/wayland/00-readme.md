# Learning Wayland

A practical, story-first introduction to the Wayland display protocol, for C programmers who want to understand how clients and compositors actually talk to each other.

A practical guide to the Wayland protocol for C programmers: selected concepts include complete, compilable C examples, and protocol exchanges are shown as they appear on the wire. The emphasis is on doing: connect, map a window, handle input, and debug the message stream when it breaks.

## Prerequisites

You need comfortable C (pointers, structs, function pointers), basic Linux tooling (`gcc`, `meson` or `make`, `pkg-config`), and a running Wayland session (Weston, Mutter, KWin, or Sway). No X11 knowledge is required.

## Scope (what this guide is not)

This guide covers the core protocol and libwayland only: objects, the wire format, `wayland.xml`, the registry, surfaces, seats, and xdg-shell.

It is NOT a libdrm, libinput, or Mesa tutorial: GPU buffer allocation, KMS modesetting, and raw input device handling belong to the compositor backend, not the protocol. It is NOT a Cairo, Pango, or GTK tutorial: client-side drawing and widget toolkits sit above the protocol and get only passing mention. The `wl_shm` software-rendering path is documented here for protocol completeness, but Flint does not use it: Flint presents via the Vulkan swapchain on a Wayland surface (see `src/vk.c`), so readers working on Flint can treat SHM details as background. The framing is primarily client-side; compositor authors will find partial utility in the wire, dispatch, and debugging chapters, but compositor internals such as rendering loops and output management are out of scope.

## Running the examples

Compile with `gcc` against libwayland-client:

```sh
gcc -o client client.c $(pkg-config --cflags --libs wayland-client)
```

Run the binary from inside a Wayland session (Weston, Mutter, KWin, or Sway) so `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` point at the compositor socket. The protocol definition lives at `/usr/share/wayland/wayland.xml`; use it (and `wayland-scanner`) as ground truth when generated headers and docs disagree.

## How to Read This Guide

Chapters 1-9 are the linear core: read them in order. Chapters 10-14 go deep on topics you can read selectively. Chapters 15-17 are reference: dip in as needed.

- **Application developers:** read 1-9, then 10-14 as needed. Do not skim 5-6: event loops and queues are where hangs come from. Skim 15-16 on a first pass.
- **Compositor developers:** read everything, with attention to 3-6 (wire, libwayland, dispatch) and 11-14 (input, popups, selection).
- **Toolkit developers:** read 2-7 closely, then 10, 11, and 13. Focus on versioning (chapter 07), queues, and resize correctness.
- Keep [17-debugging.md](17-debugging.md) open alongside 3-6 from the start; `WAYLAND_DEBUG=1` is how you confirm each step.

## Chapters

| Chapter | What it covers |
|---------|----------------|
| [01-history.md](01-history.md) | Why Wayland exists: X11 pain points and the design bets Wayland makes. |
| [02-architecture.md](02-architecture.md) | Clients, compositors, and the division of labor between them. |
| [03-wire-protocol.md](03-wire-protocol.md) | Objects, requests, events, and the binary message framing on the socket. |
| [04-interfaces-xml-patterns.md](04-interfaces-xml-patterns.md) | How `wayland.xml` defines interfaces and how code is generated from it. |
| [05-libwayland.md](05-libwayland.md) | What libwayland-client and libwayland-server do for you, and what they do not. |
| [06-display-eventloop.md](06-display-eventloop.md) | Connecting to the display, queues, dispatch, and writing a correct event loop. |
| [07-registry.md](07-registry.md) | Binding globals, version negotiation, and writing future-proof bind code. |
| [08-surfaces-shm.md](08-surfaces-shm.md) | Your first window: `wl_surface`, shared-memory buffers, and `wl_shell`-free mapping. |
| [09-xdg-basics.md](09-xdg-basics.md) | Minimal `xdg_wm_base` toplevel: ping/pong, configure/ack, and first mapped window. |
| [10-surfaces-in-depth.md](10-surfaces-in-depth.md) | Frame callbacks, damage, opaque regions, scaling, and multi-buffer patterns. |
| [11-input.md](11-input.md) | Seats, keyboards, pointers, touch, serials, and correct input object lifecycle. |
| [12-input-example.md](12-input-example.md) | A complete seat-aware client that moves and resizes with pointer and keyboard. |
| [13-xdg-in-depth.md](13-xdg-in-depth.md) | Popups, grabs, fullscreen, maximize, and the full configure state machine. |
| [14-clipboard-dnd.md](14-clipboard-dnd.md) | `wl_data_device` copy/paste and drag-and-drop, MIME types, and serial discipline. |
| [15-extensions.md](15-extensions.md) | Survey of stable, staging, and unstable extensions: viewporter, decorations, idle, presentation, and more. |
| [16-writing-extensions.md](16-writing-extensions.md) | How to design, version, and upstream your own protocol extension. |
| [17-debugging.md](17-debugging.md) | `WAYLAND_DEBUG=1`, protocol log reading, and common hangs and crashes. |

## Conventions Used

Code blocks are complete C programs or complete diffs against the running example. Serial and version notes say where each value comes from and what breaks if you get it wrong. Protocol traffic is shown as `WAYLAND_DEBUG=1` logs next to the C call that produced it. For example, the call:

```c
struct wl_registry *registry = wl_display_get_registry(display);
```

produces the log line:

```
[1234567.890]  -> wl_display@1.get_registry(new id wl_registry@2)
```
