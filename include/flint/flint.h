#ifndef FLINT_FLINT_H
#define FLINT_FLINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FL_VERSION_MAJOR 0
#define FL_VERSION_MINOR 1

// fl_app_t is opaque. Its struct is defined
// in src/ only, so its size and layout can
// change without breaking the ABI. Callers
// hold the pointer and never dereference it.
typedef struct fl_app* fl_app_t;

typedef enum fl_result {
    FL_OK = 0,
    FL_ERR_NODISPLAY = 1,
    FL_ERR_NOPROTOCOL = 2,
    FL_ERR_NOMEM = 3,
    FL_ERR_NOVULKAN = 4,
    // FL_ERR_NOSYS means the operation
    // is not implemented.
    FL_ERR_NOSYS = 5,
} fl_result_t;

// fl_app_create allocates a new app object.
// Returns FL_OK and sets *out on success.
[[nodiscard]] fl_result_t
fl_app_create(fl_app_t* out);
// fl_app_destroy releases the app object.
// Passing NULL is a no-op.
void fl_app_destroy(fl_app_t app);
// fl_app_frame pumps one Wayland round and
// draws one frame. Returns false when the
// app should exit.
[[nodiscard]] bool fl_app_frame(fl_app_t app);

#ifdef __cplusplus
}
#endif

#endif
