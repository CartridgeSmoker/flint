# Chapter 6: wl_display and the Event Loop

> **Objectives.** By the end of this chapter you will be able to:
> - Describe the `wl_display` interface: requests `sync` and `get_registry`, event `error`; recognise `delete_id` as an internal event.
> - Connect a client with `wl_display_connect`, including `WAYLAND_DISPLAY` lookup and fd variants, and disconnect with `wl_display_disconnect`.
> - Bring up a minimal server with `wl_display_create`, `add_socket_auto`, `run`, `terminate`, and `destroy` (plus `wl_display_destroy_clients`).
> - Drive the server `wl_event_loop` and embed it in your own main loop.
> - Write the correct client poll loop: flush before sleep, dispatch on `POLLIN`, roundtrip for setup.

`wl_display` is object 1, the only object both sides get for free. The client asks it for the registry, the server binds its socket to it, and `wl_display.error` explains why a connection died.

## 1. The `wl_display` interface

Open `wayland.xml` and `wl_display` is two requests and two events, all handled internally by libwayland so you never emit or parse them by hand.

**Requests (client to server):**

- `sync` takes a `new_id` for a `wl_callback`. The server emits `wl_callback.done` once it has processed everything sent before the `sync`, so `sync` is a barrier. `wl_display_roundtrip` wraps exactly this.
- `get_registry` takes a `new_id` for a `wl_registry`. This is the doorway to everything else; the next chapter lives inside it. Invoke it as infrequently as possible: the server keeps the registry resource (and everything bound through it) alive until disconnect, so one call per connection is the norm.

**Events (server to client):**

- `error(object_id, code, message)` fires when the server kills your connection: bad arguments, reused dead objects, no memory.
- `delete_id(id)` is purely internal: the server's ack that an object id is dead and the client may safely reuse that numeric id. You never send it and never handle it yourself; libwayland consumes it to recycle ids.

The standard error codes are shared across interfaces:

```c
enum {
    WL_DISPLAY_ERROR_INVALID_OBJECT     = 0,
    WL_DISPLAY_ERROR_INVALID_METHOD     = 1,
    WL_DISPLAY_ERROR_NO_MEMORY          = 2,
    WL_DISPLAY_ERROR_IMPLEMENTATION     = 3,
};
```

`invalid_object` is an unknown ID, `invalid_method` an unknown opcode, `no_memory` a server allocation failure, `implementation` a compositor-side bug. Print all three fields in your error listener; without `object_id` you cannot tell which object tripped the server.

The true listener signatures (from the generated protocol headers) are:

```c
void (*error)(void *data,
              struct wl_display *wl_display,
              void *object_id,
              uint32_t code,
              const char *message);
void (*delete_id)(void *data,
                  struct wl_display *wl_display,
                  uint32_t id);
```

`object_id` in the `error` event is an opaque proxy *pointer*, not a numeric id. Print it with `%p`; to get the numeric id call `wl_proxy_get_id((struct wl_proxy *)object_id)`. Never print it with `%u`.

## 2. Client: connecting

The whole connect program fits on one screen:

```c
#include <stdio.h>
#include <wayland-client.h>

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "wl_display_connect failed\n");
        return 1;
    }

    printf("connected, fd=%d\n", wl_display_get_fd(display));

    wl_display_disconnect(display);
    return 0;
}
```

Build and run it:

```sh
cc -o connect connect.c -lwayland-client
./connect
```

Passing `NULL` means "use the environment". The fallback is:

1. the explicit `name` argument, if non-`NULL`;
2. else `$WAYLAND_DISPLAY`, if set;
3. else the literal `"wayland-0"`.

The chosen name is resolved as `$XDG_RUNTIME_DIR/<name>` (explicit `"wayland-0"` works the same way). Inside a session (Weston, Sway, Mutter) the program prints a fd; over plain `ssh` or with `XDG_RUNTIME_DIR` unset it returns `NULL` because there is no compositor.

Its counterpart is `wl_display_disconnect(display)`, which closes the connection and frees the client-side display object.

Two fd variants cover handoff:

- `wl_display_connect_to_fd(int fd)` wraps an already-connected socket fd.
- `wl_display_get_fd(display)` returns the fd for `poll()`. Never `read()`/`write()` it directly.

## 3. Server: listening

The server skeleton mirrors the client:

```c
#include <stdio.h>
#include <wayland-server.h>

int main(void)
{
    struct wl_display *display = wl_display_create();
    if (!display) {
        fprintf(stderr, "wl_display_create failed\n");
        return 1;
    }

    const char *socket = wl_display_add_socket_auto(display);
    if (!socket) {
        fprintf(stderr, "add_socket_auto failed\n");
        wl_display_destroy(display);
        return 1;
    }

    printf("listening on %s\n", socket);
    wl_display_run(display);

    wl_display_destroy(display);
    return 0;
}
```

```sh
cc -o server server.c -lwayland-server
./server
```

Three ways to bind the socket:

- `wl_display_add_socket_auto(display)` picks the first free name from `wayland-0` upward, skipping taken names. Use it for development.
- `wl_display_add_socket(display, "wayland-1")` binds a fixed name, failing if taken. Use it for predictable test harnesses.
- `wl_display_add_socket_fd(display, fd)` adopts a bound fd, for systemd activation or a parent-owned path.

Create and bind are separate steps to give the compositor time to configure globals and state before any client can connect: nothing is reachable until you add (and eventually run) the socket.

```c
void wl_display_terminate(struct wl_display *display);
```

`wl_display_run(display)` blocks: accept clients, dispatch requests, flush replies, until `wl_display_terminate(display)` (signal handler or quit path). Then `wl_display_destroy(display)` unlinks the socket and frees clients and the loop. `wl_display_destroy_clients(display)` (destroy all clients without tearing down the display) is the finer-grained counterpart used at shutdown paths.

Test with the section 2 client. Run `./server` (prints `listening on wayland-1` when `wayland-0` is taken), then in another terminal:

```sh
WAYLAND_DISPLAY=wayland-1 ./connect
```

If the names match the client prints `connected`. Forgetting the override talks to your session compositor instead, a classic debugging trap. Keep the server in the foreground while testing, but note `Ctrl-C` does *not* exercise the terminate path: the skeleton installs no signal handler, so the default `SIGINT` disposition kills the process immediately without `wl_display_terminate`/`wl_display_destroy`. To exercise the clean path, wire the signal through the loop:

```c
static int
on_sigint(int signum, void *data)
{
    wl_display_terminate((struct wl_display *)data);
    return 0;
}

/* after wl_display_create(): */
struct wl_event_loop *loop = wl_display_get_event_loop(display);
wl_event_loop_add_signal(loop, SIGINT, on_sigint, display);
```

A production compositor does exactly this (usually for both `SIGINT` and `SIGTERM`) instead of exiting abruptly.

## 4. Server event loop

`wl_display_run` is a convenience loop. The real object is `struct wl_event_loop`, one per display:

```c
struct wl_event_loop *loop = wl_display_get_event_loop(display);
```

Sources attach to it. The header defines four mask bits: `WL_EVENT_READABLE | WL_EVENT_WRITABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR`.

- `wl_event_loop_add_fd(loop, fd, mask, cb, data)` watches any combination of the four masks. Update the mask later with `wl_event_source_fd_update(source, mask)`; drop the source with `wl_event_source_remove(source)`.
- `wl_event_loop_add_timer(loop, cb, data)` plus `wl_event_source_timer_update(source, ms_delay)` (delay in milliseconds, `int`) schedules frame and idle timeouts.
- `wl_event_loop_add_signal(loop, signum, cb, data)` handles `SIGINT`/`SIGTERM` safely.
- `wl_event_loop_add_idle(loop, cb, data)` runs once per iteration for deferred work; `wl_event_loop_dispatch_idle(loop)` runs pending idle callbacks immediately.

Callback types (return values matter: `0` normally; `1` requests a re-check only when the source is registered with `wl_event_source_check`):

```c
typedef int  (*wl_event_loop_fd_func_t)(int fd, uint32_t mask, void *data);
typedef int  (*wl_event_loop_timer_func_t)(void *data);
typedef int  (*wl_event_loop_signal_func_t)(int signal_number, void *data);
typedef void (*wl_event_loop_idle_func_t)(void *data);
```

```
 server loop (wl_display_run)
 +-------------------------------+
 | poll client fds + timers +    |
 |   signals + idle sources      |
 +---------------+---------------+
                 v
         dispatch requests
                 |
                 v
         flush replies to
         all clients
                 |
                 +--> repeat until
                      wl_display_terminate
```

`run` is "dispatch plus flush until terminated". With your own main loop, skip `run` and integrate:

```c
struct wl_event_loop *loop = wl_display_get_event_loop(display);
int loop_fd = wl_event_loop_get_fd(loop);

/* in your own poll(): watch loop_fd alongside your fds */
wl_event_loop_dispatch(loop, 0);
wl_display_flush_clients(display);
```

`wl_event_loop_get_fd` is readable when the loop has work; `dispatch` processes due sources and `wl_display_flush_clients` pushes replies. That pair reproduces `run` under your scheduler. `wl_event_loop_dispatch(loop, timeout)` takes a poll-style timeout in milliseconds: `0` is non-blocking (returns immediately), `-1` blocks. The snippet above passes `0`, so gate it with your own `poll()` on `loop_fd` first or it busy-loops.

At the other extreme you can disregard the Wayland loop entirely and manually accept and process clients on your own fds. Upstream maintainers have acknowledged the server-side event loop as a design overstep, but a useful one: most compositors just use it.

## 5. Client loop: no loop object

libwayland-client has no event loop: a client generally has only one fd (the display connection), so you own the poller and libwayland owns the queue:

- `wl_display_flush(display)` pushes buffered requests. Call it before sleeping or they sit in userspace.
- `wl_display_dispatch(display)` reads events and invokes listeners.
- `wl_display_roundtrip(display)` sends `sync`, waits for `done`. Setup barrier only, never the steady-state pump.

The trivial pump, fine for toys and tests, is one line:

```c
while (wl_display_dispatch(display) != -1)
    ;
```

It blocks inside `dispatch` with no way to watch other fds, handle timeouts, or flush proactively, so any real client graduates to the steady-state loop below:

```
 client loop (your poller)
 +-------------------------------+
 | wl_display_flush              |  push pending requests
 +---------------+---------------+
                 v
         poll(display_fd, POLLIN)
                 |
            +----+----+
            | POLLIN? |
            +----+----+
            yes /   \ no (timeout / signal)
               /     \--> loop around
              v
   wl_display_dispatch  (invoke listeners)
              |
              +--> repeat until quit
```

Full code with `poll()`:

```c
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <wayland-client.h>

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "connect failed\n");
        return 1;
    }

    /* ... bind registry, do roundtrip setup here ... */

    struct pollfd pfd;
    pfd.fd = wl_display_get_fd(display);
    pfd.events = POLLIN;

    int running = 1;
    while (running) {
        if (wl_display_flush(display) < 0) {
            if (errno == EPIPE) {
                fprintf(stderr, "compositor hung up\n");
                break;
            }
            /* EAGAIN on non-blocking fd just means poll and retry. */
        }

        if (poll(&pfd, 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (pfd.revents & POLLIN) {
            if (wl_display_dispatch(display) < 0) {
                fprintf(stderr, "dispatch failed\n");
                break;
            }
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "socket closed\n");
            break;
        }
    }

    wl_display_disconnect(display);
    return 0;
}
```

Call `wl_display_roundtrip(display)` once after `get_registry` binding so globals are resolved before use. After that, stay in flush-poll-dispatch.

> **Misconception: "`dispatch` flushes too, so I can skip `flush`."** No. `dispatch` reads incoming events; it does not push your queue. The order is flush-then-poll-then-dispatch. Skip the flush and you deadlock: `poll` waits for a reply to a request still in your own send buffer. Flush before sleeping, dispatch only on `POLLIN`.

## Recap

`wl_display` is the root: `sync` is the barrier, `get_registry` the doorway (call once per connection), `error` names the object that killed you, `delete_id` is a library-internal ack-for-reuse you recognise but never touch. Clients use `connect(NULL)` plus flush-poll-dispatch with one setup roundtrip and `disconnect` to tear down; servers use `create`, `add_socket_auto`/`add_socket`/`add_socket_fd` (separate from create so you can configure before clients connect), `run` until `terminate`, `destroy_clients`/`destroy`, and fd/timer/signal/idle sources. Almost there: the registry is next, turning this bare connection into named globals.

## Exercises

1. **Connect program.** Build `connect.c` above with `cc -o connect connect.c -lwayland-client`. Run it in your session, then as `WAYLAND_DISPLAY=does-not-exist ./connect` and `env -u XDG_RUNTIME_DIR ./connect`. Record both failures, then add an error listener printing `object_id` with `%p` (numeric id only via `wl_proxy_get_id((struct wl_proxy *)object_id)`), `code`, and `message`.
2. **Socket test.** Run the server so it reports `listening on wayland-1`, connect with `WAYLAND_DISPLAY=wayland-1 ./connect`, then switch the server to `wl_display_add_socket(display, "wayland-test")` and repeat with `WAYLAND_DISPLAY=wayland-test ./connect`. Terminate the server and note what the client prints.
