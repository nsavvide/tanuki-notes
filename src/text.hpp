#pragma once

// Text that stays sharp at any zoom: small text uses NanoVG's glyph atlas,
// large text is drawn as vector glyph outlines (an atlas can't hold a
// 5000 px glyph).

#include "geom.hpp"

#include <nanovg.h>

#include <string>
#include <vector>

namespace text {

enum Font { Hand, Ui, UiMedium, Mono };

constexpr double line_height = 1.25; // times font size, like Excalidraw

bool init(NVGcontext* vg);

// Advance width of `s` at `px` font size.
double measure(NVGcontext* vg, Font f, const std::string& s, double px);

// A line of laid-out text: bytes [begin, end) of the source string.
struct Span {
    size_t begin, end;
};

// Splits on '\n', then word-wraps to `max_w` px if max_w > 0.
std::vector<Span> wrap(NVGcontext* vg, Font f, const std::string& s, double px,
                       double max_w);

// Draws one line with its top-left at (x, y_top). Skips glyphs outside
// `viewport`.
void draw_line(NVGcontext* vg, Font f, const std::string& s, double x,
               double y_top, double px, NVGcolor color, Rect viewport);

} // namespace text
