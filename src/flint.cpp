#include "include/flint/flint.h"
#include "vk.h"
#include "wl.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

struct fl_app {
    struct Wl wl;
    struct Vk vk;
};

extern "C" {

fl_result_t fl_app_create(fl_app_t* out) {
    if (out == NULL) {
        return FL_ERR_NOMEM;
    }

    struct fl_app* app =
        (struct fl_app*)malloc(sizeof(*app));
    if (!app) {
        return FL_ERR_NOMEM;
    }
    memset(app, 0, sizeof(*app));

    if (!wl_init(&app->wl)) {
        std::cerr << "failed to wl_init()\n";
        free(app);
        return FL_ERR_NODISPLAY;
    }

    if (!vk_init(&app->vk, &app->wl)) {
        std::cerr << "failed to vk_init()\n";
        wl_finish(&app->wl);
        free(app);
        return FL_ERR_NODISPLAY;
    }

    *out = (fl_app_t)app;
    return FL_OK;
}

void fl_app_destroy(fl_app_t app) {
    if (!app) {
        return;
    }

    vk_finish(&app->vk);
    wl_finish(&app->wl);

    free(app);
}
}