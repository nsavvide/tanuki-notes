#pragma once

#include "geom.hpp"
#include "node.hpp"
#include "text.hpp"

#include <set>
#include <vector>

struct NVGcontext;
class Assets;

struct RenderCtx {
    NVGcontext* vg;
    Assets& assets;
    Rect viewport;
    float pixel_ratio = 1;
    std::set<const Node*> faded; // e.g. nodes under the eraser
    double dt = 1 / 60.0;        // seconds since the last frame
    bool animating = false;      // out: a label is still moving
};

// Parent placement + child's fractions -> child placement.
inline Xform child_xform(Xform parent, const Node& child) {
    return {parent.ox + child.x * parent.s, parent.oy + child.y * parent.s,
            parent.s * child.w};
}

inline Rect screen_rect(const Node& n, Xform t) {
    return {t.ox, t.oy, t.s, t.s * n.aspect()};
}

// Screen pixels per PARENT local unit (stroke width and font size live there).
inline double parent_scale(const Node& n, Xform t) { return t.s / n.w; }

// Points of a Line/Arrow/Draw node on screen.
std::vector<Vec2> screen_points(const Node& n, Xform t);

// Text layout shared by drawing and the text caret.
struct TextLayout {
    std::vector<text::Span> lines;
    std::vector<double> line_x; // left edge of each line
    double top = 0;             // y of the first line
    double px = 0;              // font size in pixels
};
TextLayout layout_text(NVGcontext* vg, const Node& n, Xform t);

// Where the caret sits for byte `pos` of the node's text (top-left, height).
struct CaretBox {
    double x, y, h;
};
CaretBox caret_box(NVGcontext* vg, const Node& n, Xform t, size_t pos);
// The node's text as it is laid out on screen, line by line.
std::vector<text::Span> screen_lines(NVGcontext* vg, const Node& n, Xform t);

// [[links]] in a line of text: byte ranges [begin, end) including brackets.
std::vector<text::Span> link_spans(std::string_view line);
constexpr uint32_t link_color = 0x6965dbff;

// Draws `node` and everything inside it that is visible.
void draw_subtree(RenderCtx& ctx, const Node& node, Xform t);

// Text node size (in parent units) from its content and font size.
void fit_text_bounds(NVGcontext* vg, Node& n);
// Same for a Code node: the card grows to fit its longest line.
void fit_code_bounds(NVGcontext* vg, Node& n);
