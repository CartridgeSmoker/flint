#ifndef FLINT_UI_H
#define FLINT_UI_H

#include "flint/flint.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// fl_widget_t is opaque. Its struct is
// defined in src/ui.cpp only, so layout
// can change without breaking the ABI.
// Callers hold the pointer and never
// dereference it.
typedef struct fl_widget* fl_widget_t;

// Line height in pixels for label leaves.
// The layout pass below gives each leaf
// this height.
static constexpr unsigned FL_WIDGET_LINE_H = 20;

// fl_widget_root_create makes a root
// container of size w by h at (0, 0).
// Returns FL_OK and sets *out on success,
// FL_ERR_NOMEM on null out or no memory.
[[nodiscard]]
fl_result_t fl_widget_root_create(unsigned w,
    unsigned h, fl_widget_t* out);
// fl_widget_label_create makes a leaf
// label with a copy of text. NULL text
// means an empty label. Bounds are zero
// until a layout pass runs. Returns FL_OK
// on success, FL_ERR_NOMEM on null out
// or allocation failure.
[[nodiscard]]
fl_result_t fl_widget_label_create(
    const char* text, fl_widget_t* out);
// fl_widget_destroy frees the widget and
// its whole subtree. Passing NULL is
// a no-op.
void fl_widget_destroy(fl_widget_t w);
// fl_widget_append moves child under
// parent. The parent takes ownership on
// success; the caller must not free the
// child after that. If it fails, the
// caller keeps the child. Returns FL_OK
// on success, FL_ERR_NOMEM on null args,
// no memory, a child that already has
// a parent, or a cycle.
[[nodiscard]]
fl_result_t fl_widget_append(
    fl_widget_t parent, fl_widget_t child);
// fl_widget_label_set_text replaces the
// label text with a copy of text. NULL
// clears to empty. Returns FL_OK on
// success, FL_ERR_NOMEM on null label
// or allocation failure.
fl_result_t fl_widget_label_set_text(
    fl_widget_t label, const char* text);
// fl_widget_layout runs a vertical stack
// pass: children get the full parent
// width and stack top to bottom, except
// a horizontal box splits the width.
// Panels reserve one title line, and
// viewport and table keep leaf sizes.
// Leaves take one line of height.
// Containers size to fit children,
// except the root keeps its size.
// Returns FL_OK on success,
// FL_ERR_NOMEM on null root.
[[nodiscard]]
fl_result_t fl_widget_layout(
    fl_widget_t root);
// fl_widget_bounds reads back computed x,
// y, w, h. Any out pointer may be NULL
// to skip it. Returns FL_OK on success,
// FL_ERR_NOMEM on null widget.
fl_result_t fl_widget_bounds(fl_widget_t w,
    int* x, int* y, unsigned* width,
    unsigned* height);
// fl_widget_box_create makes a box
// container. horizontal selects row vs
// column stacking. Implemented:
// geometry stacks children along the
// orientation. Returns FL_OK and sets
// *out on success, FL_ERR_NOMEM on
// null out or no memory.
[[nodiscard]]
fl_result_t fl_widget_box_create(
    bool horizontal, fl_widget_t* out);
// fl_widget_panel_create makes a panel
// with a copy of title. NULL title
// means untitled. Implemented: layout
// reserves one title line then stacks
// children vertically. Returns FL_OK
// on success, FL_ERR_NOMEM on null
// out or allocation failure.
[[nodiscard]]
fl_result_t fl_widget_panel_create(
    const char* title, fl_widget_t* out);
// fl_widget_viewport_create makes a
// viewport leaf. Implemented: layout
// gives a sane default size. Returns
// FL_OK on success, FL_ERR_NOMEM on
// null out or allocation failure.
[[nodiscard]]
fl_result_t fl_widget_viewport_create(
    fl_widget_t* out);
// fl_widget_table_create makes a table
// leaf with rows initial rows.
// Implemented: layout sizes to rows
// times one line. Returns FL_OK on
// success, FL_ERR_NOMEM on null out
// or allocation failure.
[[nodiscard]]
fl_result_t fl_widget_table_create(
    unsigned rows, fl_widget_t* out);
// fl_box_set_spacing sets the gap in
// pixels between box children.
// Implemented. Returns FL_OK on
// success, FL_ERR_NOMEM on null box
// or a non-box widget.
fl_result_t fl_box_set_spacing(
    fl_widget_t box, unsigned spacing);
// fl_panel_set_dock attaches docking
// behavior to a panel. Stub: always
// returns FL_ERR_NOSYS.
fl_result_t fl_panel_set_dock(
    fl_widget_t panel, int dock);
// fl_viewport_render draws the viewport
// contents. Stub: always returns
// FL_ERR_NOSYS.
fl_result_t fl_viewport_render(
    fl_widget_t viewport);
// fl_table_set_cell sets the text of one
// table cell. Stub: always returns
// FL_ERR_NOSYS.
fl_result_t fl_table_set_cell(
    fl_widget_t table, unsigned row,
    unsigned col, const char* text);

#ifdef __cplusplus
}
#endif

#endif
