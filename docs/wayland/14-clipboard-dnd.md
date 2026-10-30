# Chapter 14: Clipboard and Drag-and-Drop

> **Objectives:** By the end of this chapter you will be able to explain the promises-plus-pipes model; get `wl_data_device` per seat and create `wl_data_source`; implement copy with `offer` + `set_selection`; implement paste with `accept` + `receive` + read-to-EOF; run a drag with `start_drag`, icons, and v3 action negotiation; avoid fd leaks, stale serials, and premature move-deletes.

## 14.1 No Bytes on the Wire

Clipboard and drag-and-drop share one mechanism: `wl_data_device_manager`. There is no central buffer. Copy sends no text. Paste queries no buffer. The source makes a *promise* ("I have `text/plain`"); the compositor later connects source to destination with a *pipe*.

Three objects split the work:

- `wl_data_source` — **yours when you copy or drag.** You create it, advertise MIME types, later write bytes.
- `wl_data_offer` — **theirs when you paste or receive.** The compositor creates one per offer. You pick a MIME type and read bytes.
- `wl_data_device` — **the referee, one per seat.** It routes selection and drag events. No bytes flow through it.

Byte transport is always a Unix fd passed in `send` or `receive`. One side writes, the other reads to EOF, both close. A leaked fd blocks the reader forever and exhausts the session.

```
COPY (promise only)              PASTE (pipe only)
[source]-offer(text/plain)->[comp]
[source]-set_selection(src)->[comp] (nothing sent)
       ...later...
       [comp]-data_offer(id)->[dest]
       [comp]-selection(id)->[dest]
       [dest]-accept(mime)->[comp]-target(mime)->[source]
       [dest]-receive(mime,fd)->[source]-send(mime,fd)
       [source]=write=>pipe=read=>[dest] (EOF, close both)
```

## 14.2 The Manager Global

`wl_data_device_manager` is a singleton with two constructors and no events:

- `get_data_device(new id wl_data_device, seat)` — one device per `wl_seat`. Call once per seat.
- `create_data_source(new id wl_data_source)` — makes an empty source you configure.

Version 3 adds DND action negotiation. Bind defensively and cap at 3:

```c
uint32_t want = version < 3 ? version : 3;
struct wl_data_device_manager *manager = wl_registry_bind(
    registry, name, &wl_data_device_manager_interface, want);
```

Remember `want`. Only call `set_actions` when `want >= 3`; otherwise the compositor kills you with a protocol error. This is the same bind-and-guard pattern as every extension (see [Chapter 15, section 3](15-extensions.md#3-bind-mechanics-are-identical)).

## 14.3 The Three Objects

**wl_data_source (yours).** Requests: `offer(mime)` once per type, faithful-first (`text/x-my-app-row` before `text/plain`, plus a `text/plain;charset=utf-8` fallback); `set_actions(copy|move|ask)` for DND only (v3); `destroy` after `cancelled` or `dnd_finished`. Events: `target(mime or NULL)` — destination's pick (NULL = declined); `send(mime, fd)` — write exactly this MIME to `fd`, then close it; `cancelled` — promise dead, free source and backing string; `dnd_drop_performed` — released over a surface, keep data; `dnd_finished` — transfer done; `action(action)` (v3) — negotiated outcome.

**wl_data_offer (theirs).** Events: `offer(mime)` once per type in source preference order; `source_actions(bits)` (v3); `action(chosen)` (v3, emitted multiple times - last value before `drop` wins; none arrives after `drop`, and on the source side `action` arrives only after `dnd_drop_performed` in special cases). Requests: `accept(serial, mime or NULL)` — always call on enter, NULL to decline; for DND the serial must come from the drag `enter` on the same seat, while for a selection paste any recent input serial on the same seat works (a fabricated or stale serial breaks `set_selection`, not `accept`); if you never accept a MIME the source sees `target(NULL)` and may never get `receive`, so always accept on enter (DND) / before `receive` (paste) — on the wire it is `receive` that triggers `send`, and `accept` advertises the pick via `target`; `receive(mime, fd)` — pass a pipe write-end; `finish()` — call only if you consumed the drop via `receive`, enables move-delete on the source; `set_actions(supported, preferred)` (v3) — your vote; `destroy` when replaced. Errors: `invalid_action_mask`, `invalid_action`, `invalid_offer`, `invalid_finish`.

**wl_data_device (referee).** Events: `data_offer(new id)` — always precedes use, store it; `enter(serial, surface, x, y, id)` — drag entered, save serial; `motion(time, x, y)`; `drop()` — button released; `leave()` — always reset highlight and drag state; `selection(id or NULL)` — clipboard changed, NULL means empty, destroy the old offer. Selection is per seat: two seats mean two independent clipboards, one `wl_data_device` each. Requests: `set_selection(source or NULL, serial)` — publish or clear (NULL source clears); `start_drag(source or NULL, origin, icon or NULL, serial)` — both `source` and `icon` allow NULL, `origin` must be the surface with pointer focus and `serial` the button-press serial from the same seat or the drag is ignored / raises a `role` error; `release()` (since 2). Errors: `role` (e.g. icon surface already has another role), `used_source` (`start_drag` with an already-used source).

## 14.4 FD Transport

`receive`/`send` carry an fd via socket ancillary data, not inline bytes. Convention: receiver calls `pipe(fds)`, passes `fds[1]` in `receive`, closes its copy after flush; source gets `send` with a fresh fd, loops `write()` handling `EINTR` and partial writes, then `close()`; receiver loops `read()` until `0`, then `close()`. Never assume one `read` gets all. Never leave the write end open elsewhere or EOF never arrives.

## 14.5 Copy: Make a Promise

Nothing moves at copy time. The Ctrl+C handler gives you a serial; save it, because `set_selection` needs a compositor-seen input serial.

```c
static uint32_t ctrl_c_serial; /* saved in key handler */

void copy_text(struct app *a, const char *text) {
    struct wl_data_source *src =
        wl_data_device_manager_create_data_source(a->manager);
    wl_data_source_offer(src, "text/plain;charset=utf-8");
    wl_data_source_offer(src, "text/plain");
    /* No set_actions here: it is since=3 drag negotiation only;
       selection copy ignores it. set_actions(COPY|MOVE) belongs
       in begin_drag (section 14.7). */
    free(a->copy_text);
    a->copy_text = strdup(text);
    a->copy_source = src;
    wl_data_source_add_listener(src, &source_listener, a);
    wl_data_device_set_selection(a->device, src, ctrl_c_serial);
    /* No bytes moved. Wait for send or cancelled. */
}

static void on_send(void *d, struct wl_data_source *s,
        const char *mime, int32_t fd) {
    struct app *a = d;
    (void) s;
    /* Honour the requested MIME: serve what we offered, reject the rest. */
    const char *payload = NULL;
    if (strcmp(mime, "text/plain;charset=utf-8") == 0 ||
        strcmp(mime, "text/plain") == 0)
        payload = a->copy_text;
    if (!payload) { close(fd); return; }
    size_t len = strlen(payload), off = 0;
    while (off < len) {
        ssize_t n = write(fd, payload + off, len - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        off += (size_t) n;
    }
    close(fd);
}

static void on_cancelled(void *d, struct wl_data_source *s) {
    struct app *a = d;
    wl_data_source_destroy(s);
    if (a->copy_source == s) {
        a->copy_source = NULL;
        /* Minimal example owns the string with the live source.
           A toolkit with clipboard history transfers ownership
           to the history snapshot instead of freeing here. */
        free(a->copy_text);
        a->copy_text = NULL;
    }
}
```

Stale serials are rejected silently: background windows cannot steal the clipboard. If copy does nothing, your serial is old or fabricated. Timer-thread silence is correct — no `send` arrives until someone pastes. Clearing also needs a serial: `set_selection(NULL, serial)`. Toolkits keep a history snapshot; the `wl_data_source` is only the live head entry.

> **Misconception callout: "`set_selection` sends text"?** No. It sends a promise: an object id plus a serial. Payload bytes move later, if ever, through a pipe the *pasting* client creates. If nobody pastes, zero payload bytes cross the socket. That is why `cancelled` must free the promise, why the backing string must outlive the source, and why `send` must honour the requested MIME — one promise can be consumed zero, one, or many times for different types.

## 14.6 Paste: Accept, Receive, Read to EOF

Store the selection offer when `selection` arrives. On Ctrl+V pick the best MIME, `accept` it, pipe, `receive`, read to EOF. Here `a->last_serial` is the serial saved from the last pointer or keyboard `enter` on the same seat; unlike `start_drag` (which requires the button-press serial from the drag origin surface on the same seat), a selection paste accepts any recent input serial on that seat.

```
[comp]-data_offer(O)->[dest] (store O)
[comp]-selection(O)->[dest]  (O is current)
...user pastes...
[dest]-accept(mime)->[comp]-target(mime)->[source]
[dest]-receive(mime,wfd)->[source]-send(mime,rfd)
[source]=write=>pipe=read=>[dest] (to EOF)
```

If you never accept a MIME the source sees `target(NULL)` and may never get `receive`, so always accept before `receive`; on the wire `receive` triggers `send`, and `accept` advertises the pick via `target`. If `selection` delivers NULL the offer is dead: destroy it and clear state.

```c
void paste_text(struct app *a) {
    if (!a->sel_offer || !a->sel_count)
        return;
    const char *mime = pick_best(a); /* prefer utf-8 over plain */
    wl_data_offer_accept(a->sel_offer, a->last_serial, mime);
    int fds[2];
    if (pipe(fds) < 0)
        return;
    int flags = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    wl_data_offer_receive(a->sel_offer, mime, fds[1]);
    close(fds[1]);
    /* Sketch, not a tight loop: a blocking read() here deadlocks a
       same-client roundtrip, because the source's send event arrives
       via the display queue. Flush, then dispatch while draining the
       nonblocking pipe (or read on a separate thread) to EOF. */
    char buf[4096], *text = malloc(1);
    size_t len = 0;
    bool eof = false;
    text[0] = '\0';
    wl_display_flush(a->display);
    while (!eof) {
        struct pollfd p = { .fd = fds[0], .events = POLLIN };
        while (poll(&p, 1, 0) > 0) {
            ssize_t n = read(fds[0], buf, sizeof buf);
            if (n == 0) { eof = true; break; }
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    break;
                eof = true;
                break;
            }
            if (len + (size_t) n > (1u << 20)) { /* 1 MiB cap: abort */
                free(text); text = NULL; eof = true; break;
            }
            char *nt = realloc(text, len + (size_t) n + 1);
            if (!nt) { free(text); text = NULL; eof = true; break; }
            text = nt;
            memcpy(text + len, buf, (size_t) n);
            len += (size_t) n;
            text[len] = '\0';
        }
        if (!eof)
            wl_display_dispatch(a->display); /* pump send to source */
    }
    close(fds[0]);
    if (text) { insert_at_cursor(a, text); free(text); }
}
```

Drops add `finish()`: after a successful `receive` + read, call `wl_data_offer_finish()` so a move-source may delete its original. A copy that skips `finish` simply leaves the source intact. Calling `finish()` after accepting NULL (or with no `action` received on v3) is a protocol error, as is calling it outside drag-and-drop (`invalid_finish`). After `finish()`, any request other than `destroy` is a protocol error.

## 14.7 Drag: Threshold, Icon, Negotiate

Start on press-plus-motion, not press alone. Precondition: `origin` must be the surface with pointer focus and `serial` the button-press serial from the same seat, else the drag is ignored or fails with a `role` error. Once motion exceeds ~8 px with the button held, build a source like a copy plus DND extras.

```c
void begin_drag(struct app *a) {
    struct wl_data_source *src =
        wl_data_device_manager_create_data_source(a->manager);
    wl_data_source_offer(src, "text/plain;charset=utf-8");
    wl_data_source_offer(src, "text/plain");
    if (a->version >= 3)
        wl_data_source_set_actions(src,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
            WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    wl_data_source_add_listener(src, &dnd_listener, a);
    /* Icon gets a dedicated role; never reuse a toplevel. */
    struct wl_surface *icon =
        wl_compositor_create_surface(a->compositor);
    draw_drag_icon(icon, a);
    wl_data_device_start_drag(a->device, src,
        a->origin, icon, a->press_serial);
    a->drag_source = src;
}
```

No serial, no drag: use the button-press serial from the same seat. Destination protocol: `enter` (save serial, hit-test, always `accept` with a MIME or NULL) → `motion` (update highlight, re-vote `set_actions` on v3) → `drop()` (`receive`, read, `finish` if consumed) → `leave()` (always reset, even after success). Source side: `action` reports the negotiated result; `dnd_drop_performed` means keep data; `dnd_finished` means done — delete moved data only here and only if `action == move`. Deleting on `drop_performed` destroys the file before the destination reads it.

```
SOURCE               COMP                 DEST
 |--start_drag------->|                     |
                      |--enter(serial..)--->|
                      |<--accept(mime)-------|
                      |--motion------------->|
                      |<--set_actions--------|
 |<--action (negotiated; may repeat, last before drop wins)
                      |--drop()------------->|
 |<--dnd_drop_performed (user dropped; NOT accepted yet; do not destroy source)
                      |<--receive-----------|
 |<--send---==pipe==>------------------------|
                      |<--finish-------------|
 |<--finished---------|<--leave()------------|
 (delete only if action==move, after finished)
```

## 14.8 Incremental Path

1. **Text copy/paste** with `text/plain` only between two of your windows.
2. **NULL-icon copy-drag:** pass NULL icon, skip `set_actions`, verify enter/motion/drop/leave.
3. **Full DND:** icon surface, action voting, `finish`, delete-on-`finished`-with-`move`, plus `text/uri-list` alongside plain text.

## Recap

Sources `offer`, destinations `accept` + `receive`, the compositor routes fds, both ends close. Serials prove focus; stale ones fail silently. `cancelled`, `leave`, and `selection-NULL` always clean up. Moves delete only on `dnd_finished` with a `move` action after the destination called `finish`.

**Exercises:**

1. **Text roundtrip.** Copy UTF-8 with emoji between two instances. Clear with `set_selection(NULL, serial)`, confirm paste does nothing and the old offer is destroyed. Trace `WAYLAND_DEBUG=1` and find `target`, `send`, EOF (see [Chapter 17, section 6](17-debugging.md#6-log-reading-healthy-vs-stalled) for log reading).
2. **NULL-icon drag.** Drag with NULL icon, `copy`-only. Decline (`accept` NULL) on the left half, accept on the right. Confirm no `send` when declined, `finish` completes the accepted path, and `leave` always clears highlight.
