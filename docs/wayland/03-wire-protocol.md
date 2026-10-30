# Chapter 3: The Wayland Wire Protocol

> **Objectives:** By the end of this chapter you will be able to (1) describe a Wayland connection as 32-bit words in host byte order, (2) encode every wire type including strings and file descriptors, (3) parse a message header into object id, size, and opcode, (4) explain client versus server object-id allocation, and (5) connect over the Unix socket via `WAYLAND_SOCKET` or `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`.

Every Wayland interaction is a message on an object. The protocol XML defines methods and signatures; this chapter defines their bytes.

## 1. A stream of 32-bit words

A connection carries a flat sequence of 32-bit words with no delimiters. Each message contains its own length, so the receiver reads words until a full message arrives, dispatches it, and starts the next. There is no framing layer. Both sides share the signature of each opcode, so they know how many words to expect.

Byte order is **host byte order**, little-endian on nearly all desktops. Client and compositor share a machine, so no byte conversion happens. Every value occupies whole words: integers take one word; strings and arrays take a length word plus bytes rounded up to a multiple of four.

## 2. The type system

Each argument has one wire type:

- **`int` / `uint`**: one word, signed or unsigned 32-bit. Coordinates are usually `int`; ids and masks are `uint`.
- **`fixed`**: one word holding signed 24.8 fixed-point: 24 bits integer, 8 bits fraction in units of 1/256. The value 10.5 is stored as 2688. No floating-point exists on the wire.
- **`object`**: one word holding a 32-bit object id, or 0 for null.
- **`new_id`**: one word holding a freshly allocated id. Requests that create objects, such as binding a global, carry one. The interface is given by an extra argument or agreed in advance for that request/event.
- **`string`**: a `uint` length plus bytes rounded up to word alignment. The length includes a trailing NUL, always present, plus zero to three undefined pad bytes (in practice often zero; do not rely on it). String encoding is unspecified, UTF-8 in practice.
- **`array`**: like a string but for opaque bytes: a `uint` count plus raw bytes rounded up, with no NUL requirement. Padding bytes are undefined (in practice often zero; do not rely on them).
- **`fd`**: 0-bit value on the wire; not in the word stream. The descriptor travels via `msg_control` ancillary data (`SCM_RIGHTS`) alongside the words; the kernel delivers it out of band.
- **`enum`**: an `int` or `uint` (or bitmap) with named constants. On the wire it is just an integer.

The signature fixes argument order. `wl_surface.damage` is `(int, int, int, int)`, so after its header come exactly four integer words. `wl_seat.get_pointer` is `(new_id)`, so after its header comes one id word.

## 3. Message header, worked end to end

Every message opens with an 8-byte header: **word 0** is the target object id; **word 1** packs total size including the header in its upper 16 bits and the opcode in its lower 16 bits. Arguments follow in signature order. Events use the same layout.

Take a client marking part of surface 12 dirty:

```
wl_surface.damage(10, 20, 480, 300)
```

On `wl_surface`, `damage` is opcode 2. Four integer arguments need 16 bytes plus the 8-byte header, so the size is 24. As decimal words on the wire:

- **Word 0: 12.** The target. The compositor looks up id 12 and finds a `wl_surface`.
- **Word 1: size 24 on top, opcode 2 below.** The top 16 bits say 24, so the next message starts 24 bytes later; the bottom 16 bits say 2, operation 2 on that interface. As one number it is 24 times 65536 plus 2, but parsers read it as two halves.
- **Word 2: 10.** Argument x.
- **Word 3: 20.** Argument y.
- **Word 4: 480.** Argument width.
- **Word 5: 300.** Argument height.

Six words, 24 bytes, no padding. The receiver splits word 1, waits for 24 bytes, matches opcode 2 against the `wl_surface` table, and decodes four integers. A short read means wait for more data, not a protocol error.

## 4. How strings pack: "hi"

Integers are trivial; strings show the padding rule. Length counts content bytes **plus NUL**, padded with undefined bytes to a word boundary (in practice often zero; do not rely on it).

Encoding `hi`:

- **Length word: 3.** Two characters plus one NUL.
- **Payload word: bytes 104, 105, 0, undefined.** That is `h`, `i`, the NUL terminator, and one undefined pad byte (often zero in practice) to fill the 4-byte word.

So `hi` costs two words. The receiver rounds the length up to a multiple of four to find the next argument and checks for trailing NUL. Arrays match, minus the NUL check.

## 5. Who picks object ids

Ids are the only shared namespace, so allocation splits the 32-bit space to avoid round trips:

- **0** means null. Never a live object.
- **1** is always `wl_display`, known before any message flows.
- **Client ids** run from 1 to `0xFEFFFFFF` (4278190079). Id 1 is the reserved `wl_display`, so the client allocates upward from 2: 2, 3, 4, and so on.
- **Server ids** run from `0xFF000000` (4278190080) to `0xFFFFFFFF` (4294967295). The server allocates upward from its own lower bound.

Each side allocates only from its half. A client creating an object picks the next free client id and sends it as `new_id`. Small ids in a log are the client; huge ids near four billion are the compositor.

## 6. One transport: the Unix socket

The specification allows any reliable transport with descriptor passing. In practice there is one: the **Unix-domain socket**. Keymaps, shared-memory pools, dma-buf handles, and clipboard streams all move as descriptors via `SCM_RIGHTS`, which only Unix sockets support.

Setup tries three sources in order:

1. **`WAYLAND_SOCKET`**: fd NUMBER of a pre-connected socket provided by the parent (e.g. `WAYLAND_SOCKET=5`).
2. **`$WAYLAND_DISPLAY` under `$XDG_RUNTIME_DIR`**: the usual path, e.g. `/run/user/1000/wayland-1`.
3. **Fallback to `wayland-0`**: tried when nothing is set.

The client speaks first, normally with `wl_display.get_registry` (see registry chapter).

> **Misconception callout: "Why not TCP?"**
>
> Because "here are 4 MB of pixels" passes an open file description, not copied bytes. `SCM_RIGHTS` duplicates the descriptor with shared pages intact: zero-copy buffers, sealed memfds for keymaps, pipes for clipboard. TCP has no such side channel. The Unix socket is the data plane, not plumbing.

## Recap

- The connection is 32-bit words in host byte order, grouped by an internal size field. No network order, no framing.
- `int`, `uint`, `fixed`, `object`, and `new_id` take one word; `string` and `array` take a length word plus bytes with undefined padding; `fd` is a 0-bit value on the wire riding out of band via `msg_control` ancillary data (`SCM_RIGHTS`); `enum` is a documented integer (or bitmap). String encoding is unspecified, UTF-8 in practice.
- Each message is an object id, a size-plus-opcode word, then arguments in order. `damage` on surface 12 is six words and 24 bytes: 12, then size 24 with opcode 2, then 10, 20, 480, 300.
- Strings count the NUL: `hi` is length 3 followed by `104, 105, 0` plus one undefined pad byte.
- Id 0 is null and 1 is `wl_display`; clients allocate up from 2 through `0xFEFFFFFF` (4278190079), servers up from `0xFF000000` (4278190080) through `0xFFFFFFFF` (4294967295), so neither collides.
- The only real transport is the Unix socket found via `WAYLAND_SOCKET` or `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY` with a `wayland-0` fallback, because only it passes descriptors.

## Exercises

1. **Watch the wire.** Run `WAYLAND_DEBUG=1 weston-simple-shm` (or any Wayland client) and capture the first fifty lines. Find the `wl_display.get_registry` request and the answering `wl_registry.global` events. For three consecutive messages, name the object id, opcode, and signature, and check each `new_id` against the client or server half.
2. **Decode by hand.** A client sends decimal words `[1, 786433, 2]` on object 1, where `wl_display` has `sync`=0 and `get_registry`=1. Split word 1 into size and opcode, name the request, explain word 2, and give the total byte length. Then state what the compositor must send next before the client can proceed, and list what globals arrive (each `wl_registry.global` carries name/interface/version and allocates nothing).
