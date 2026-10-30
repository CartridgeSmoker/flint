#include "flint/ui.h"

#include <memory>
#include <string>
#include <vector>

// Kind tags the retained node. Root is
// the sized container from create;
// Label is a one-line leaf; Box stacks
// along one axis; Panel keeps one title
// line above a vertical stack;
// Viewport and Table are sized leaves.
enum class Kind {
    Root,
    Label,
    Box,
    Panel,
    Viewport,
    Table,
};

// fl_widget is the retained node. Only
// fl_widget_t crosses the C boundary;
// all members below stay in C++.
struct fl_widget {
    Kind kind;
    bool is_root;
    std::string text;
    bool horizontal;
    unsigned spacing;
    unsigned rows;
    int x;
    int y;
    unsigned w;
    unsigned h;
    fl_widget* parent;
    std::vector<std::unique_ptr<
        fl_widget>> children;
};

namespace {

// kViewportH is the default viewport
// leaf height in pixels. Tables use
// rows times FL_WIDGET_LINE_H.
constexpr unsigned kViewportH = 200;

// leaf_height gives the childless size
// for a widget. The root keeps its
// created height.
[[nodiscard]]
unsigned leaf_height(
    fl_widget* w) noexcept {
    switch (w->kind) {
    case Kind::Root:
        return w->h;
    case Kind::Panel:
        return FL_WIDGET_LINE_H;
    case Kind::Viewport:
        return kViewportH;
    case Kind::Table:
        if (w->rows == 0) {
            return FL_WIDGET_LINE_H;
        }
        return w->rows *
            unsigned(FL_WIDGET_LINE_H);
    case Kind::Box:
        return 0;
    case Kind::Label:
    default:
        return FL_WIDGET_LINE_H;
    }
}

// layout_at places w at (x, y) with width
// wd, then recurses into children. It
// returns the height w occupies. The root
// keeps its created height; any other
// container sizes to fit its children,
// except viewport and table, which keep
// their leaf height even when they have
// children. A vertical box stacks full
// width rows with spacing between them;
// a horizontal box splits the width
// evenly with spacing gaps and takes
// the tallest child height. A panel
// starts children below one title line.
unsigned layout_at(fl_widget* w, int x,
    int y, unsigned wd) noexcept {
    w->x = x;
    w->y = y;
    w->w = wd;
    if (w->children.empty()) {
        if (!w->is_root) {
            w->h = leaf_height(w);
        }
        return w->h;
    }
    if (w->kind == Kind::Viewport ||
        w->kind == Kind::Table) {
        unsigned keep = leaf_height(w);
        unsigned cursor = 0;
        for (auto& kid : w->children) {
            unsigned kh = layout_at(
                kid.get(), x,
                y + int(cursor), wd);
            cursor += kh;
        }
        w->h = keep;
        return keep;
    }
    if (w->kind == Kind::Panel) {
        unsigned cursor = 0;
        for (auto& kid : w->children) {
            unsigned kh = layout_at(
                kid.get(), x,
                y + int(unsigned(
                            FL_WIDGET_LINE_H) +
                        cursor),
                wd);
            cursor += kh;
        }
        if (!w->is_root) {
            w->h = unsigned(
                    FL_WIDGET_LINE_H) +
                cursor;
        }
        return w->h;
    }
    if (w->kind == Kind::Box &&
        w->horizontal) {
        unsigned n = unsigned(
            w->children.size());
        unsigned gaps = 0;
        if (n > 1) {
            gaps = w->spacing * (n - 1);
        }
        unsigned avail = 0;
        if (gaps < wd) {
            avail = wd - gaps;
        }
        unsigned base = 0;
        unsigned extra = 0;
        if (n > 0) {
            base = avail / n;
            extra = avail % n;
        }
        unsigned maxh = 0;
        unsigned cursor = 0;
        unsigned i = 0;
        for (auto& kid : w->children) {
            unsigned cw = base;
            if (i < extra) {
                cw += 1;
            }
            unsigned kh = layout_at(
                kid.get(),
                x + int(cursor), y, cw);
            if (kh > maxh) {
                maxh = kh;
            }
            cursor += cw + w->spacing;
            i += 1;
        }
        if (!w->is_root) {
            w->h = maxh;
        }
        if (w->is_root) {
            return w->h;
        }
        return maxh;
    }
    unsigned cursor = 0;
    unsigned n = 0;
    for (auto& kid : w->children) {
        if (n > 0 &&
            w->kind == Kind::Box) {
            cursor += w->spacing;
        }
        unsigned kh = layout_at(kid.get(),
            x, y + int(cursor), wd);
        cursor += kh;
        n += 1;
    }
    if (!w->is_root) {
        w->h = cursor;
    }
    if (w->is_root) {
        return w->h;
    }
    return cursor;
}

// is_descendant reports whether anc is w
// itself or sits above w in the parent
// chain. Used to reject cycles.
bool is_descendant(fl_widget* w,
    fl_widget* anc) noexcept {
    for (; w != nullptr; w = w->parent) {
        if (w == anc) {
            return true;
        }
    }
    return false;
}

} // namespace

// fl_widget_root_create allocates a root
// container. C linkage comes from ui.h.
fl_result_t fl_widget_root_create(
    unsigned w, unsigned h,
    fl_widget_t* out) {
    try {
        if (out == nullptr) {
            return FL_ERR_NOMEM;
        }
        auto node =
            std::make_unique<fl_widget>();
        node->kind = Kind::Root;
        node->is_root = true;
        node->horizontal = false;
        node->spacing = 0;
        node->rows = 0;
        node->x = 0;
        node->y = 0;
        node->w = w;
        node->h = h;
        node->parent = nullptr;
        *out = node.release();
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_widget_label_create allocates a leaf
// label holding a copy of text.
fl_result_t fl_widget_label_create(
    const char* text, fl_widget_t* out) {
    try {
        if (out == nullptr) {
            return FL_ERR_NOMEM;
        }
        auto node =
            std::make_unique<fl_widget>();
        node->kind = Kind::Label;
        node->is_root = false;
        node->horizontal = false;
        node->spacing = 0;
        node->rows = 0;
        if (text != nullptr) {
            node->text = text;
        }
        node->x = 0;
        node->y = 0;
        node->w = 0;
        node->h = 0;
        node->parent = nullptr;
        *out = node.release();
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_widget_destroy frees a widget and its
// subtree. Members cannot throw, so there
// is nothing to catch here.
void fl_widget_destroy(fl_widget_t w) {
    delete w;
}

// fl_widget_append transfers child into
// the parent. reserve() runs first so a
// vector growth failure leaves the child
// with the caller, not half moved.
fl_result_t fl_widget_append(
    fl_widget_t parent, fl_widget_t child) {
    try {
        if (parent == nullptr) {
            return FL_ERR_NOMEM;
        }
        if (child == nullptr) {
            return FL_ERR_NOMEM;
        }
        if (child->parent != nullptr) {
            return FL_ERR_NOMEM;
        }
        if (is_descendant(parent, child)) {
            return FL_ERR_NOMEM;
        }
        parent->children.reserve(
            parent->children.size() + 1);
        parent->children.emplace_back(
            child);
        child->parent = parent;
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_widget_label_set_text swaps in a copy
// of text; NULL clears to empty.
fl_result_t fl_widget_label_set_text(
    fl_widget_t label, const char* text) {
    try {
        if (label == nullptr) {
            return FL_ERR_NOMEM;
        }
        if (text == nullptr) {
            label->text.clear();
        } else {
            label->text = text;
        }
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_widget_layout stacks the tree under
// root top to bottom at full width.
// Inner boxes, panels, viewports, and
// tables lay out per their kind.
fl_result_t fl_widget_layout(
    fl_widget_t root) {
    try {
        if (root == nullptr) {
            return FL_ERR_NOMEM;
        }
        layout_at(root, root->x, root->y,
            root->w);
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_widget_bounds copies out computed
// bounds; NULL outs are skipped.
fl_result_t fl_widget_bounds(
    fl_widget_t w, int* x, int* y,
    unsigned* width, unsigned* height) {
    try {
        if (w == nullptr) {
            return FL_ERR_NOMEM;
        }
        if (x != nullptr) {
            *x = w->x;
        }
        if (y != nullptr) {
            *y = w->y;
        }
        if (width != nullptr) {
            *width = w->w;
        }
        if (height != nullptr) {
            *height = w->h;
        }
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_widget_box_create allocates a box
// container with the given orientation.
fl_result_t fl_widget_box_create(
    bool horizontal, fl_widget_t* out) {
    try {
        if (out == nullptr) {
            return FL_ERR_NOMEM;
        }
        auto node =
            std::make_unique<fl_widget>();
        node->kind = Kind::Box;
        node->is_root = false;
        node->horizontal = horizontal;
        node->spacing = 0;
        node->rows = 0;
        node->x = 0;
        node->y = 0;
        node->w = 0;
        node->h = 0;
        node->parent = nullptr;
        *out = node.release();
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_widget_panel_create allocates a
// panel holding a copy of title.
fl_result_t fl_widget_panel_create(
    const char* title, fl_widget_t* out) {
    try {
        if (out == nullptr) {
            return FL_ERR_NOMEM;
        }
        auto node =
            std::make_unique<fl_widget>();
        node->kind = Kind::Panel;
        node->is_root = false;
        node->horizontal = false;
        node->spacing = 0;
        node->rows = 0;
        if (title != nullptr) {
            node->text = title;
        }
        node->x = 0;
        node->y = 0;
        node->w = 0;
        node->h = 0;
        node->parent = nullptr;
        *out = node.release();
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_widget_viewport_create allocates a
// viewport leaf with a default size.
fl_result_t fl_widget_viewport_create(
    fl_widget_t* out) {
    try {
        if (out == nullptr) {
            return FL_ERR_NOMEM;
        }
        auto node =
            std::make_unique<fl_widget>();
        node->kind = Kind::Viewport;
        node->is_root = false;
        node->horizontal = false;
        node->spacing = 0;
        node->rows = 0;
        node->x = 0;
        node->y = 0;
        node->w = 0;
        node->h = 0;
        node->parent = nullptr;
        *out = node.release();
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_widget_table_create allocates a
// table leaf sized by rows rows.
fl_result_t fl_widget_table_create(
    unsigned rows, fl_widget_t* out) {
    try {
        if (out == nullptr) {
            return FL_ERR_NOMEM;
        }
        auto node =
            std::make_unique<fl_widget>();
        node->kind = Kind::Table;
        node->is_root = false;
        node->horizontal = false;
        node->spacing = 0;
        node->rows = rows;
        node->x = 0;
        node->y = 0;
        node->w = 0;
        node->h = 0;
        node->parent = nullptr;
        *out = node.release();
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_box_set_spacing records the gap in
// pixels between box children.
fl_result_t fl_box_set_spacing(
    fl_widget_t box, unsigned spacing) {
    try {
        if (box == nullptr) {
            return FL_ERR_NOMEM;
        }
        if (box->kind != Kind::Box) {
            return FL_ERR_NOMEM;
        }
        box->spacing = spacing;
        return FL_OK;
    } catch (...) {
        return FL_ERR_NOMEM;
    }
}

// fl_panel_set_dock is a stub. Docking
// is future work, so it reports NOSYS.
fl_result_t fl_panel_set_dock(
    fl_widget_t panel, int dock) {
    (void)panel;
    (void)dock;
    return FL_ERR_NOSYS;
}

// fl_viewport_render is a stub. GPU
// drawing is future work, so it
// reports NOSYS.
fl_result_t fl_viewport_render(
    fl_widget_t viewport) {
    (void)viewport;
    return FL_ERR_NOSYS;
}

// fl_table_set_cell is a stub. Cell
// storage is future work, so it
// reports NOSYS.
fl_result_t fl_table_set_cell(
    fl_widget_t table, unsigned row,
    unsigned col, const char* text) {
    (void)table;
    (void)row;
    (void)col;
    (void)text;
    return FL_ERR_NOSYS;
}
