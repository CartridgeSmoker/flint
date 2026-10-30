# Tutorial: your first Flint window

This guide takes you from zero to a
Flint window with a widget on screen.
It follows the real code in `main.c`,
`include/flint/flint.h`, and
`include/flint/ui.h`.

## Prerequisites

Install deps and check your toolchain
first. See [developing.md](developing.md)
for the per-distro commands, then:

```sh
make        # build ./bin/flint
```

You need a C23 compiler, Wayland client
libs, and Vulkan headers. Debug builds
also want the Vulkan validation layers
package. Details live in developing.md.

## The smallest app

Create `hello.c` next to `main.c`:

```c
#include <stdio.h>

#include "flint/flint.h"

int main(void) {
    fl_app_t app = nullptr;
    fl_result_t rc =
        fl_app_create(&app);
    if (rc != FL_OK) {
        if (rc ==
            FL_ERR_NODISPLAY) {
            printf("no display, "
                "skipping\n");
            return (0);
        }
        fprintf(stderr, "failed to "
            "create app\n");
        return (1);
    }
    for (long left = 120;
            left > 0; left--) {
        if (!fl_app_frame(app)) {
            break;
        }
    }
    fl_app_destroy(app);
    return (0);
}
```

Build it the same way `Makefile`
builds `main.c`: compile with
`-Iinclude`, link with Wayland and
Vulkan libs, and link with `$(CXX)`
so the UI object resolves libstdc++.
Simplest start: replace `main.c`
temporarily, or copy its compile line.

The shipped `main.c` is this pattern
plus `--frames` parsing. Run it with:

```sh
./bin/flint --frames 120
```

## What each call does

* `fl_app_create(&app)` connects to
Wayland, waits for the first configure,
then sets up Vulkan. See below for
the handshake. On success it sets
`*out` and returns `FL_OK`.
* `fl_app_frame(app)` pumps one
Wayland round with `wl_dispatch`,
then draws one frame with
`vk_draw_frame`. It returns false
when the app should exit: the
compositor asked to close, dispatch
failed, or a redraw failed.
* `fl_app_destroy(app)` tears down in
reverse order: swapchain, surface,
device, instance, then Wayland
objects. Passing `NULL` is a no-op.

## What can fail

* `FL_ERR_NODISPLAY` means no Wayland
display was found. Treat it as a
headless skip, not a bug: print
`no display, skipping` and exit 0.
`make check` relies on this.
* Any other code is a real error.
`FL_ERR_NOVULKAN` means Vulkan setup
failed. `FL_ERR_NOMEM` means a null
out pointer or no memory. Exit 1.
* `fl_app_frame` signals failure by
returning false. Break the loop and
destroy the app. Do not retry in
a tight loop.

## Add a widget

Widgets live in `include/flint/ui.h`
and are CPU side only for now. There
is no call yet that attaches a widget
tree to a window, so this snippet
builds a tree, runs layout, and prints
bounds. Drawing it comes later.

```c
#include <stdio.h>

#include "flint/ui.h"

int main(void) {
    fl_widget_t root = nullptr;
    fl_widget_t label = nullptr;
    if (fl_widget_root_create(640,
            480, &root) != FL_OK) {
        return (1);
    }
    if (fl_widget_label_create("hello",
            &label) != FL_OK) {
        fl_widget_destroy(root);
        return (1);
    }
    if (fl_widget_append(root,
            label) != FL_OK) {
        fl_widget_destroy(label);
        fl_widget_destroy(root);
        return (1);
    }
    /* label now belongs to root */
    fl_widget_layout(root);
    int x = 0, y = 0;
    unsigned w = 0, h = 0;
    fl_widget_bounds(label, &x, &y,
        &w, &h);
    printf("label at %d,%d %ux%u\n",
        x, y, w, h);
    fl_widget_destroy(root);
    return (0);
}
```

What each call does:

* `fl_widget_root_create(640, 480,
&root)` makes a 640 by 480 container
at (0, 0). The root keeps its size
during layout.
* `fl_widget_label_create("hello",
&label)` copies the text into a leaf.
`NULL` text means an empty label.
Bounds stay zero until layout runs.
* `fl_widget_append(root, label)`
moves the child under the parent.
On success the parent owns the child:
destroy the root only. On failure
you still own the child, so free it.
* `fl_widget_layout(root)` stacks
children top to bottom at full parent
width. Each label leaf takes one line
of `FL_WIDGET_LINE_H` (20 px), so the
label above gets bounds `0,0 640x20`.
* `fl_widget_bounds(...)` reads back
x, y, w, h. Any out pointer may be
`NULL` to skip it.
* `fl_widget_destroy(root)` frees the
whole subtree. `NULL` is a no-op.

Failures return `FL_ERR_NOMEM`: null
args, no memory, a child that already
has a parent, or a cycle. Check every
return. Stubs such as
`fl_panel_set_dock`,
`fl_viewport_render`, and
`fl_table_set_cell` return
`FL_ERR_NOSYS`. Avoid them for now.

## Run it

Headless (no compositor, no
`WAYLAND_DISPLAY`, e.g. SSH or CI):

```sh
./bin/flint --frames 120
# prints: no display, skipping
# exit code 0, check passes
```

With a compositor (Weston, Sway,
Labwc, or similar):

```sh
./bin/flint --frames 120
# opens a window titled "Flint"
# draws 120 frames, then exits
```

Omit `--frames` to run until you close
the window. `N` must be over 0. This
bounded form is what `make check`
uses, and it keeps scripts and CI
from hanging.

## Why resize just works

First show: `wl_init` commits an empty
surface, then blocks until the first
`xdg_surface` configure arrives and is
acked. Presenting before that point is
a protocol error, so create waits.

Later resizes: a nonzero
`xdg_toplevel` configure stores the new
size and sets a `resized` flag.
`fl_app_frame` sees the flag and calls
`vk_swapchain_create` again, which
drops the old swapchain and rebuilds
it for the new size. If a frame
reports `OUT_OF_DATE` or `SUBOPTIMAL`,
the frame sets `resize_needed` and the
swapchain is rebuilt the same way.
Your loop does nothing special.

## Exercises

1. Resize the window and watch
recreate. Run without `--frames`
under a compositor, drag a window
edge, and confirm the app keeps
running. Each resize rebuilds the
swapchain inside `fl_app_frame`.
2. Add a second label and print
bounds. Append another
`fl_widget_label_create` child,
rerun layout, and print both sets
of bounds. Expect the second label
at y 20 with the same width.
3. Run with `--frames`. Try
`./bin/flint --frames 5` headless
and under a compositor. Confirm
the skip message and exit 0 with
no display, and a short bounded
run with a display.
