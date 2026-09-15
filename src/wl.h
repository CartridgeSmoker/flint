#ifndef WL_H
#define WL_H

#include <stdbool.h>
#include <wayland-client.h>

// Wl holds the needed wayland connections.
struct Wl {
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_compositor* compositor;
    struct xdg_wm_base* wm_base;
    struct wl_surface* surface;
};

// wl_init connects, and binds the compositor,
// xdg wm base, and creates the surface.
// Returns true on success and false on failure.
bool wl_init(struct Wl* wl);

// wl_finish disconnects and destroys the surface.
void wl_finish(struct Wl* wl);

#endif
