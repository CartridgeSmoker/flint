# Chapter 2: How a Wayland Desktop Fits Together

> **Objectives:** By the end of this chapter you will be able to (1) trace the stack from hardware through kernel to userspace to compositor to client, (2) explain GPU rendering vs scanout and why tearing happens, (3) distinguish DRM primary nodes from render nodes and evdev nodes, and (4) state what a compositor does, what a client does, and why global keylogging and screenshots are impossible via the core protocol without a privileged extension plus user approval.

> **Scope note:** This guide covers the Wayland protocol plus libwayland only. It is not a libdrm, libinput, or Mesa tutorial - those libraries appear here just enough to show where Wayland sits. Everything is framed from the client's point of view: what your app sends, receives, and can assume.

A Wayland desktop feels like one thing, but it is a stack of layers. We climb bottom-up: hardware, kernel, userspace, compositor, clients. Picture the compositor as receptionist in the middle. Clients never touch hardware.

## 1. Hardware: Rendering, Scanout, and Raw Reports

A modern GPU does two jobs. **Rendering** computes pixel values: shaders, triangles, blending. **Scanout** reads a finished framebuffer from memory and streams it to the monitor on a strict schedule, typically 60 or 144 times per second. Rendering may finish early or late. Scanout cannot wait.

**Tearing** is the two colliding. If rendering overwrites the buffer mid-scanout, the top shows the new frame and the bottom the old one. The fix: render into a back buffer, then swap during the blanking interval (vsync - the pause between monitor refreshes when a new frame may be swapped in without tearing). More in Chapter 10.

Input hardware is dumber. A USB keyboard sends **HID reports** like "usage 0x04 pressed", not letters. No uppercase, no layout, no target app. Mice send deltas; touchscreens send positions. Interpretation happens higher up.

## 2. Kernel: DRM/KMS and evdev

Linux exposes this hardware through two subsystems, both visible as device nodes. Hardware has little concept of apps: it does what it is told, so exactly one owner talks to it on each path.

Graphics is **DRM/KMS** (Direct Rendering Manager / Kernel Mode Setting) under `/dev/dri/`:

- `/dev/dri/card0`: **primary node**. Privileged. Sets modes, commits framebuffers. Only the display owner, normally the compositor via logind/seat, opens card0 for modesetting.
- `/dev/dri/renderD128`: **render node**. Unprivileged. Submits GPU jobs, allocates buffers. GPU-accelerated clients MAY use renderD128; SHM/software clients never open DRM.

A game can render furiously through `renderD128` without reprogramming your monitor. One owner - the compositor - drives the display through `card0`.

Input is **evdev** (`/dev/input/event*`). Each device emits `struct input_event` records (`EV_KEY`, `EV_REL`, `EV_ABS`). Still raw: `KEY_A` means "the key in that position", not "a". Only the compositor reads these. Clients never open them.

## 3. Userspace: The Libraries in the Middle

The compositor rarely issues ioctls by hand. It calls libraries:

- **libdrm** wraps DRM ioctls: allocate buffers, set modes, queue page flips.
- **Mesa (OpenGL/Vulkan) + GBM** makes pixels. **GBM** allocates buffers shareable between GPU and display. On the dma-buf path (dma-buf - a Linux kernel mechanism for passing buffer handles between drivers without copying pixels) this is often zero-copy handle sharing: a client renders into a buffer and passes a dma-buf fd to the compositor, which imports the same pages. SHM clients copy instead, and even dma-buf import can linearize (force a copy) when modifiers or formats do not match.
- **libinput** normalizes input. It reads evdev nodes, applies acceleration, palm detection, tap-to-click, and emits clean events: "key 30 pressed", "pointer +dx, +dy". Here "key 30" is a scancode (a raw positional number for the physical key, evdev `KEY_A` = 30) - not yet a letter.
- **udev** announces hotplug and owns device enumeration and permissions. Plug in a keyboard, the kernel creates a node, udev tells libinput. No restart. Its rules also decide which group or seat may open `/dev/input/event*` and `/dev/dri/*` in the first place.
- **xkbcommon** turns scancodes into keysyms (a keysym is the symbolic meaning, e.g. `a` vs `A` vs `ä`, after layout and modifiers are applied). libinput says "key 30 down"; xkbcommon adds the standard +8 offset (evdev 30 becomes xkb keycode 38, a historical X11 compatibility quirk), consults the keymap plus its **state machine** (Shift? Caps Lock? AltGr? dead key?) and yields `A` or `ä`. Layout lives here. xkbcommon was extracted from Xorg code but has no practical X relation at runtime - it is just the shared keymap library.
- **pixman** is the CPU fallback for blending when there is no GPU.
- **libwayland** is the socket transport and reference implementation: the C `libwayland-client` / `libwayland-server` libraries plus the `wayland-scanner` XML code-generation tool that turns `wayland.xml` into language bindings. Chapter 5 covers it in full.
- **GTK / Qt / SDL / GLFW** hide all of this. `gtk_window_new()` opens the socket, negotiates `wl_compositor` and `xdg_wm_base`, and handles frame callbacks behind your back. Many apps do not know about Wayland at all - they draw through one of these toolkits and let it speak the protocol.

**wlroots** gives small compositors (Sway, Hyprland) a ready backend: KMS, libinput, renderer, xdg-shell. **Mutter (GNOME)** and **KWin (KDE)** are in-house: same protocol, hand-built for their desktops.

The Wayland protocol itself is conservative and abstract: it deals in surfaces, buffers, seats, and frames, not GPUs, evdev nodes, or keymaps. A desktop could run most apps without implicating most of this stack - a client that draws with SHM and reads keysyms off its socket never touches DRM, GBM, libinput, or xkbcommon directly. Those layers matter when you debug tearing, input, or zero-copy, but they are not the protocol.

### What's in the Wayland package

Installing Wayland (e.g. `wayland`, `libwayland-dev`) yields the most popular implementation, not the only one: `libwayland-client` and `libwayland-server` in `/usr/lib`, headers in `/usr/include`, the `wayland-scanner` code generator in `/usr/bin`, and the canonical `wayland.xml` protocol description in `/usr/share/wayland`. That XML file is a pre-compiled copy of the core protocol - scanners for other languages (notably `wayland-rs`, or standalone generators like `wayland-monad`) read the same XML to produce their own bindings, so nothing forces you to use the C library.

## 4. The Full Picture

Read bottom-up for output (pixels to screen), top-down for input (devices to app). Note that libwayland is not a middlebox: it is a library linked into *both* sides, with `libwayland-client` inside each client and `libwayland-server` inside the compositor, talking over a Unix socket:

```
  +------------------+  +------------------+  +------------------+
  |  Client A (GTK)  |  |  Client B (Qt)   |  |  Client C (SDL)  |
  |  private buffers |  |  private buffers |  |  private buffers |
  | + libwayland-    |  | + libwayland-    |  | + libwayland-    |
  |   client linked  |  |   client linked  |  |   client linked  |
  +--------+---------+  +--------+---------+  +--------+---------+
           | Unix socket ($XDG_RUNTIME_DIR/$WAYLAND_DISPLAY)
           |  -- attach buffers (client -> compositor, output) -->
           |  <-- key/pointer/frame events (compositor -> client, input) --
  +--------v----------------------------------------------------+
  |        COMPOSITOR + libwayland-server linked in             |
  |        1. composite buffers    2. KMS commit @ vsync        |
  |        3. input dispatch                                   |
  +-----+-------------------------------+----------------------+
        ^ input path: evdev ->            | output path: Mesa/libdrm/
          libinput/udev/xkbcommon         | GBM/pixman -> DRM/KMS commit v
  +--------------------------v--------v-----------------------+
  | KERNEL: DRM/KMS (card0 vs renderD128) + evdev (event*)   |
  +--------------------------+-------------------------------+
                             |
  +--------------------------v-------------------------------+
  | HARDWARE: GPU render vs scanout, monitor, USB HID        |
  +----------------------------------------------------------+
```

Note the missing arrows: no client-to-client, no client-to-screen, no client-to-input. Clients reach only the compositor. That placement is the security model.

## 5. Compositor vs Client

A **compositor** has exactly three jobs:

1. **Composite.** Blend each client's latest buffer into one image: stacking, clipping, opacity, scaling.
2. **Commit on vsync.** Hand that image to KMS at the blanking interval, so scanout never catches a half-written frame.
3. **Dispatch input as receptionist.** Take libinput events, **hit-test** (determining which surface is under the pointer?), check **focus** (who owns the keyboard?), forward to that client only. No broadcast.

A **client** owns only **private buffers**. It draws its window, offers a new frame, and receives only its own input. It cannot list other clients or read their buffers. The protocol has no such request.

That is why **keylogging and screenshots are impossible via the core protocol without a privileged extension plus user approval**. Each client gets its own registry; the compositor MAY filter what it advertises, and events route only to the focused or hovered surface. There is no `get_keystrokes()` or `get_screen_pixels()`. An app sees keys pressed *while focused* only. Screenshots need a privileged path (`xdg-desktop-portal`, `wlr-screencopy`) with compositor and usually user approval.

## 6. Worked Example: A Keypress from USB to App

Trace Shift+a producing `A` in a focused app (a terminal, say - the pty is just what that app happens to do with the text; any app follows the same input path):

1. You press the key. The keyboard sends a HID report: "usage 0x04 + modifier down".
2. The kernel decodes it; evdev emits `input_event { EV_KEY, KEY_A (30), 1 }` on `/dev/input/event3`.
3. libinput emits "key 30 pressed, Shift active" to the compositor.
4. The compositor checks focus - that app owns the keyboard - and feeds keycode 38 (evdev 30 + the standard xkb +8 offset) plus Shift into xkbcommon (`us` map). It returns keysym `A`.
5. The compositor sends `wl_keyboard.key` over that client's socket only.
6. The toolkit renders the `A` via SHM or Mesa/dmabuf and commits with `wl_surface.attach`. (In a terminal, the toolkit first writes "A" to its pty and redraws the terminal grid - that pty step is terminal-specific, not part of the Wayland path.)
7. The compositor composites and commits via libdrm to `card0` at vsync. The `A` appears, tear-free.

Seven hops, three privilege boundaries, no point where another app could see the key.

> **Misconception callout: "So the compositor is just the window manager?"**
>
> Not quite. An X11 window manager arranges windows while a separate X server draws them and owns input. A Wayland compositor fuses all three: display server *plus* window manager *plus* input router in one process. It composites every pixel, drives KMS, and dispatches every event. That fusion is the security model: no separate all-seeing server to query for global state.

## Recap

- Hardware splits rendering (computing pixels) from scanout (streaming them); tearing is a half-updated buffer caught mid-scanout.
- The kernel splits graphics into privileged `card0` and unprivileged `renderD128`, and exposes raw input as evdev nodes.
- Userspace translates: libdrm, Mesa/GBM (often zero-copy on the dma-buf path, copies for SHM), libinput plus udev enumeration/permissions, xkbcommon state (+8 keycode offset), pixman fallback, libwayland transport (C libraries + scanner), GTK/Qt/SDL/GLFW (many apps never touch Wayland directly), wlroots or in-house Mutter/KWin.
- The compositor composites, commits on vsync, dispatches by hit-test plus focus. Clients own only private buffers. Isolation holds at the core-protocol level: per-client registries the compositor may filter, no global queries - privileged extensions plus user approval are the only way around it.

## Exercises

1. **Look at your own nodes.** Run `ls -l /dev/dri/` and `ls -l /dev/input/event*`. Which are `card*` vs `renderD*`? What group owns the input nodes - can your apps open them? Then run `ls -l $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY` (or `ls $XDG_RUNTIME_DIR/wayland-*`). What socket connects clients to the compositor, and what are its permissions?
2. **List the globals (preview).** In Chapter 7 you will build a globals lister; peek now. If `weston-info` or `wayland-info` is installed, run it and count the globals (`wl_compositor`, `xdg_wm_base`, `wl_seat`, ...). Guess: does every client see the same list, or could the compositor filter per client? Keep your answer for Chapter 7.
