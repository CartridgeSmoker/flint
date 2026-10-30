#include "flint/flint.h"
#include "vk.h"
#include "wl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fl_app {
    struct Wl wl;
    struct Vk vk;
};

#ifdef __cplusplus
extern "C" {
#endif

// fl_app_create connects Wayland, then sets
// up Vulkan. Cleans up on failure.
fl_result_t fl_app_create(fl_app_t* out) {
    if (out == nullptr) {
        return FL_ERR_NOMEM;
    }

    *out = nullptr; // ensure caller sees NULL on failure

    struct fl_app* app =
        (struct fl_app*)malloc(sizeof(*app));
    if (!app) {
        return FL_ERR_NOMEM;
    }
    memset(app, 0, sizeof(*app));

    if (!wl_init(&app->wl)) {
        fprintf(stderr, "failed to wl_init()\n");
        free(app);
        return FL_ERR_NODISPLAY;
    }

    if (!vk_init(&app->vk, &app->wl)) {
        fprintf(stderr, "failed to vk_init()\n");
        vk_finish(&app->vk);
        wl_finish(&app->wl);
        free(app);
        return FL_ERR_NOVULKAN;
    }

    *out = (fl_app_t)app;
    return FL_OK;
}

// fl_app_frame pumps one Wayland round and
// draws one frame. Returns false when the
// app should exit.
bool fl_app_frame(fl_app_t app) {
    if (!app) {
        return false;
    }
    struct fl_app* p = (struct fl_app*)app;
    if (!wl_dispatch(&p->wl)) {
        return false;
    }
    if (p->wl.closed) {
        return false;
    }
    if (p->wl.resized) {
        if (!vk_swapchain_create(&p->vk,
                &p->wl)) {
            return false;
        }
        p->wl.resized = false;
    }
    bool resize_needed = false;
    if (!vk_draw_frame(&p->vk,
            &resize_needed)) {
        return false;
    }
    if (resize_needed) {
        if (!vk_swapchain_create(&p->vk,
                &p->wl)) {
            return false;
        }
        p->wl.resized = false;
    }
    return true;
}

// fl_app_destroy releases the app object.
// Passing NULL is a no-op.
void fl_app_destroy(fl_app_t app) {
    if (!app) {
        return;
    }

    vk_finish(&app->vk);
    wl_finish(&app->wl);

    free(app);
}

#ifdef __cplusplus
}
#endif