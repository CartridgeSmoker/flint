# Chapter 17: Debugging Wayland Clients and Compositors

Most Wayland bugs are ordering bugs: a missing ack, a bind that never happened, a destroy in the wrong order. `WAYLAND_DEBUG=1` shows all of it.

## 17.1 WAYLAND_DEBUG=1: read every message

Log every request and event with interface, opcode, object id, args, fds, and serials. `WAYLAND_DEBUG` takes `client`, `server`, or `1` (both sides); `0` disables:

```sh
WAYLAND_DEBUG=1 ./my-client 2>&1 | tee way.log
WAYLAND_DEBUG=client ./my-client 2>&1 | tee way.log
WAYLAND_DEBUG=server ./my-compositor 2>&1 | tee comp.log
# client-only when compositor is noisy:
WAYLAND_DEBUG=client ./my-client 2>&1 | grep -E "xdg_surface|wl_buffer|wl_display|error"
```

What you get per line: direction (`->` request, `<-` event), interface + method, object ids, args, serials. Reproduce small, find the last good exchange, read the next 20 lines.

Signatures:

- Missing ack: `xdg_surface.configure(serial=N)` repeats with climbing serials, no `ack_configure` follows. Surface never commits new state. (See [Chapter 13](13-xdg-in-depth.md) for the configure/ack state machine.)
- Wrong destroy order: `xdg_popup.destroy` after parent destroy. Log ends with `wl_display.error`. Destroy top-down (popup before its parent toplevel). Note: `wl_buffer.destroy` while attached is legal - it frees only the client proxy; the server retains the committed contents until `wl_buffer.release` (SHM pixels live in the pool). The real buffer bug is rewriting or reusing the SHM pool storage, or destroying the pool, before release: keep one pool per size class and destroy each `wl_buffer` in its `release` handler.
- Stale selection serial: `set_selection(source, serial=OLD)` where `OLD` is not the last enter serial. Compositor ignores it; paste silently fails. (See [Chapter 14](14-clipboard-dnd.md) for serial and fd discipline.)

## 17.2 Globals inventory: confirm exposure before blaming binds

If `registry_bind` fails, the global was likely never advertised. List what the compositor exposes first:

```sh
cc -o /tmp/globals globals.c -lwayland-client
WAYLAND_DEBUG=0 /tmp/globals
ls /usr/share/wayland/wayland.xml
```

Expected output is one line per global: `name id interface version`.

Compositor equivalents (`wayland-info` comes with `wayland-utils`; `weston-debug` streams compositor-side debug scopes):

```sh
wayland-info | head -n 60
weston-info | head -n 60
weston-debug --help
swaymsg -t get_outputs; ls -l "$XDG_RUNTIME_DIR"/wayland-*
```

Checklist (see [Chapter 7](07-registry.md) for the globals-lister and bind flow these items come from):

- [ ] Does the interface appear at all?
- [ ] Advertised version `>=` requested? Clamp: `min(want, advertised)`.
- [ ] Did you roundtrip after `get_registry`? Without `wl_display.roundtrip`, the list is empty.
- [ ] On mismatch, does the client degrade instead of aborting?

Bind `min(client_max, advertised)` and branch. Build against v7 `xdg_wm_base` (the currently installed `xdg-shell.xml`) but still run against v2+, minus newer requests.

## 17.3 wayland.xml is ground truth

Docs rot; the installed XML does not:

```sh
ls /usr/share/wayland/wayland.xml
ls /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
grep -A8 'name="ack_configure"' /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
pkg-config --variable=pkgdatadir wayland-protocols
```

XML wins over docs. Check version, names, arg types, and `allow-null`/enum notes. Generated headers must come from the same XML the compositor implements.

## 17.4 Socket triage

No connection means no protocol:

```sh
echo "DISPLAY=$WAYLAND_DISPLAY RUNTIME=$XDG_RUNTIME_DIR"
ls -l "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/"
ls -l "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
ss -x | grep -i wayland
```

Checklist:

- [ ] `XDG_RUNTIME_DIR` set and owned by you (`drwx------`, not root)?
- [ ] `WAYLAND_DISPLAY` set? Default `wayland-0`; `wayland-1` means a second compositor.
- [ ] Socket exists and is a socket (`srwxr-xr-x`)? If missing, the compositor is not running here.
- [ ] SSH/container without compositor? Immediate `failed to connect`. Forward the socket or run headless (`weston --backend=headless`, `cage`) instead of debugging protocol.

Trap: `sudo` wipes both variables. Preserve them explicitly.

## 17.5 Black-window checklist

A mapped but black window means the handshake stalled. Walk in order (configure/ack steps detailed in [Chapter 13](13-xdg-in-depth.md)):

- [ ] Connected? Socket triage clean.
- [ ] Registry roundtrip done?
- [ ] Bound all three? `wl_compositor` + `wl_shm`/dmabuf + `xdg_wm_base`.
- [ ] Empty commit sent after `get_toplevel` to trigger first configure?
- [ ] Configure arrived (`configure(serial=N)`)?
- [ ] Ack sent for the latest serial, then commit?
- [ ] One `attach` + `damage` + `commit` per frame? Missing damage is the classic SHM black window.
- [ ] Role assigned exactly once? Second `get_toplevel`/`get_popup` is `role already assigned`.
- [ ] Responding to `xdg_wm_base.ping` with `pong`? An unresponsive client looks hung and may be marked unresponsive/killed.
- [ ] `set_window_geometry` empty or sane? An empty or wrong geometry can leave nothing visible; unset it to check.

If any step is absent, fix that step.

## 17.6 Log reading: healthy vs stalled

Healthy configure/ack exchange (annotated):

```
<- xdg_surface#12.configure(serial=42)   # compositor proposes state
-> xdg_surface#12.ack_configure(serial=42) # client accepts THAT serial
-> wl_surface#10.attach(buffer#20, 0, 0) # client supplies content
-> wl_surface#10.damage_buffer(0, 0, 256, 256)
-> wl_surface#10.commit()                # ack + content commit together
<- wl_surface#10.frame(callback#30)      # compositor schedules next frame
```

Missing-ack stall (annotated):

```
<- xdg_surface#12.configure(serial=42)   # first proposal, no ack follows
<- xdg_surface#12.configure(serial=43)   # compositor retries / resizes
<- xdg_surface#12.configure(serial=44)   # serials climb, client silent
-> wl_surface#10.attach(buffer#20, 0, 0) # client draws but never acked
-> wl_surface#10.commit()                # commit without ack: compositor keeps old state
# symptom: window stays black or frozen at initial size; fix is ack_configure(44) then commit
```

Protocol errors end the connection. Read the last line first. Note `code` in `wl_display.error` is per-interface: look up the error enum on that object's interface in the XML (see [Chapter 13](13-xdg-in-depth.md) for `xdg_surface` role/configure codes and [Chapter 14](14-clipboard-dnd.md) for data-device/seat codes):

```
<- wl_display#1.error(object#12, code=4, message="xdg_surface has already been assigned a role")
```

Triage:

- [ ] Copy object/code/message verbatim; it names the offender.
- [ ] Second-role? Remove the duplicate `get_toplevel`/`get_popup`.
- [ ] Popup destroy? Destroy popup before its parent toplevel.
- [ ] Newer request on older bind? Client called a `since=N` request on a bind whose version is older. A correct compositor never sends a `since=2` event to a v1 bind (events are gated on `wl_resource_get_version`); the fatal direction is client-to-server, and the compositor kills the connection with `wl_display.error`. Gate every version-gated request with `bound_version >= N`.

## 17.7 FD hygiene

Leaked fds hang after many pastes or frames. (See [Chapter 14](14-clipboard-dnd.md) for the serial/fd discipline these rules come from.)

- [ ] Selection: one pipe pair per paste; close write end after send, read end on finish.
- [ ] SHM: one pool per size class, `munmap` + close on resize, not per frame.
- [ ] Every `release`/`destroy` removes its listener; no callbacks into freed data.
- [ ] Verify: `ls -l /proc/$(pidof my-client)/fd | wc -l` plateaus, not grows per frame.

```sh
ls -l /proc/$(pidof my-client)/fd
strace -f -e trace=%network,close,mmap,munmap -p $(pidof my-client) 2>&1 | head -n 40
```

## 17.8 Five-minute minimal repro

```sh
cc -o /tmp/minimal minimal.c -lwayland-client -lwayland-egl
WAYLAND_DEBUG=1 /tmp/minimal 2>&1 | tee /tmp/min.log
grep -n "configure\|ack_configure\|attach\|commit\|error\|global" /tmp/min.log | head -n 40
ls -l "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
```

Recipe: start from the globals-lister in [Chapter 7](07-registry.md), add only `wl_compositor` + `xdg_wm_base` + one surface + one SHM buffer. Add your feature back in halves until it breaks. File 20 log lines around the failure plus globals list and XML versions.

## Recap

Debug order: socket, globals, handshake, content, cleanup. Trust `wayland.xml`, never bind above advertised, keep a sub-100-line repro that maps a window.
