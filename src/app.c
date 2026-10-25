#include "flint/flint.h"
#include "wl.h"
#include "vk.h"
#include <stdlib.h>

struct fl_app {
    struct Wl wl;
    struct Vk vk;
};

fl_result_t fl_app_create(fl_app_t* out) {
    if (!out) return FL_ERR_NOMEM;
    struct fl_app* app = calloc(1, sizeof(*app));
    if (!app) return FL_ERR_NOMEM;
    if (!wl_init(&app->wl)) {
        free(app);
        return FL_ERR_NODISPLAY;
    }
    if (!vk_init(&app->vk, &app->wl)) {
        wl_finish(&app->wl);
        free(app);
        return FL_ERR_NOPROTOCOL;
    }
    *out = app;
    return FL_OK;
}

void fl_app_destroy(fl_app_t app) {
    if (!app) return;
    vk_finish(&app->vk);
    wl_finish(&app->wl);
    free(app);
}