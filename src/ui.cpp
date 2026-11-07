#include "ui.hpp"

#include <numbers>

namespace ui {

void island(NVGcontext* vg, Rect r) {
    float x = float(r.x), y = float(r.y), w = float(r.w), h = float(r.h);
    NVGpaint shadow = nvgBoxGradient(vg, x, y + 2, w, h, 10, 12,
                                     nvgRGBA(0, 0, 0, 28), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, x - 12, y - 10, w + 24, h + 26);
    nvgRoundedRect(vg, x, y, w, h, 8);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 8);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgFill(vg);
    nvgStrokeColor(vg, rgb(line));
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
}

void button_bg(NVGcontext* vg, Rect r, bool active, bool hovered) {
    if (!active && !hovered) {
        return;
    }
    nvgBeginPath(vg);
    nvgRoundedRect(vg, float(r.x), float(r.y), float(r.w), float(r.h), 6);
    nvgFillColor(vg, rgb(active ? primary_light : hover));
    nvgFill(vg);
}

float label(NVGcontext* vg, const std::string& s, double x, double y,
            float size, NVGcolor c, bool medium, int align) {
    nvgFontFace(vg, medium ? "ui-medium" : "ui");
    nvgFontSize(vg, size);
    nvgTextAlign(vg, align);
    nvgFillColor(vg, c);
    return nvgText(vg, float(x), float(y), s.c_str(), nullptr) - float(x);
}

void swatch(NVGcontext* vg, Rect r, uint32_t rgba, bool active) {
    float x = float(r.x), y = float(r.y), w = float(r.w), h = float(r.h);
    if (active) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x - 2, y - 2, w + 4, h + 4, 6);
        nvgStrokeColor(vg, rgb(primary));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 4);
    if ((rgba & 0xff) == 0) { // transparent: checker hint
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgFill(vg);
        nvgStrokeColor(vg, rgb(0xced4da));
        nvgStrokeWidth(vg, 1);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, x + 3, y + h - 3);
        nvgLineTo(vg, x + w - 3, y + 3);
        nvgStrokeColor(vg, rgb(0xe03131));
        nvgStroke(vg);
        return;
    }
    nvgFillColor(vg, nvgRGBA((rgba >> 24) & 0xff, (rgba >> 16) & 0xff,
                             (rgba >> 8) & 0xff, 255));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(0, 0, 0, 25));
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
}

// Icons are drawn in a 20x20 design box centered in `box`.
void icon(NVGcontext* vg, Icon which, Rect box, NVGcolor c) {
    nvgSave(vg);
    nvgTranslate(vg, float(box.x + box.w / 2 - 10), float(box.y + box.h / 2 - 10));
    nvgStrokeColor(vg, c);
    nvgFillColor(vg, c);
    nvgStrokeWidth(vg, 1.5f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgBeginPath(vg);
    auto line_ = [&](float x0, float y0, float x1, float y1) {
        nvgMoveTo(vg, x0, y0);
        nvgLineTo(vg, x1, y1);
    };
    switch (which) {
    case Icon::Hand:
        nvgRoundedRect(vg, 5, 8, 10, 9, 3);
        line_(7, 8, 7, 3.5f);
        line_(9.5f, 8, 9.5f, 2.5f);
        line_(12, 8, 12, 3.5f);
        line_(14.5f, 9, 14.5f, 5.5f);
        break;
    case Icon::Select:
        nvgMoveTo(vg, 5, 3);
        nvgLineTo(vg, 15, 11);
        nvgLineTo(vg, 10.5f, 12);
        nvgLineTo(vg, 13, 17);
        nvgLineTo(vg, 11, 18);
        nvgLineTo(vg, 8.5f, 13);
        nvgLineTo(vg, 5, 16);
        nvgClosePath(vg);
        break;
    case Icon::Rect:
        nvgRoundedRect(vg, 3.5f, 4.5f, 13, 11, 2.5f);
        break;
    case Icon::Diamond:
        nvgMoveTo(vg, 10, 2.5f);
        nvgLineTo(vg, 17.5f, 10);
        nvgLineTo(vg, 10, 17.5f);
        nvgLineTo(vg, 2.5f, 10);
        nvgClosePath(vg);
        break;
    case Icon::Ellipse:
        nvgCircle(vg, 10, 10, 7);
        break;
    case Icon::Arrow:
        line_(3.5f, 16.5f, 16.5f, 3.5f);
        line_(16.5f, 3.5f, 9.5f, 4);
        line_(16.5f, 3.5f, 16, 10.5f);
        break;
    case Icon::Line:
        line_(3.5f, 10, 16.5f, 10);
        break;
    case Icon::Draw:
        nvgMoveTo(vg, 4, 16);
        nvgLineTo(vg, 5, 12.5f);
        nvgLineTo(vg, 13.5f, 4);
        nvgLineTo(vg, 16, 6.5f);
        nvgLineTo(vg, 7.5f, 15);
        nvgClosePath(vg);
        line_(11.5f, 6, 14, 8.5f);
        break;
    case Icon::Text:
        line_(4, 5, 16, 5);
        line_(10, 5, 10, 16);
        line_(7.5f, 16, 12.5f, 16);
        break;
    case Icon::Image:
        nvgRoundedRect(vg, 3, 4, 14, 12, 2);
        nvgMoveTo(vg, 3, 14);
        nvgLineTo(vg, 8, 9);
        nvgLineTo(vg, 13, 14);
        nvgMoveTo(vg, 11, 12);
        nvgLineTo(vg, 13, 10);
        nvgLineTo(vg, 17, 14);
        nvgCircle(vg, 13, 7.5f, 1.2f);
        break;
    case Icon::Eraser:
        nvgMoveTo(vg, 8, 16);
        nvgLineTo(vg, 3.5f, 11.5f);
        nvgLineTo(vg, 11, 4);
        nvgLineTo(vg, 16.5f, 9.5f);
        nvgLineTo(vg, 10, 16);
        nvgClosePath(vg);
        line_(6.5f, 8.5f, 12, 14);
        line_(8, 16, 16, 16);
        break;
    case Icon::Undo:
    case Icon::Redo: {
        bool undo = which == Icon::Undo;
        float s = undo ? 1 : -1, cx = 10;
        auto X = [&](float x) { return cx + (x - cx) * s; };
        nvgMoveTo(vg, X(4), 8);
        nvgLineTo(vg, X(12), 8);
        nvgBezierTo(vg, X(15.5f), 8, X(16.5f), 10.5f, X(16.5f), 12);
        nvgBezierTo(vg, X(16.5f), 14, X(15), 16, X(12), 16);
        nvgLineTo(vg, X(8), 16);
        line_(X(4), 8, X(7.5f), 4.5f);
        line_(X(4), 8, X(7.5f), 11.5f);
        break;
    }
    case Icon::Plus:
        line_(5, 10, 15, 10);
        line_(10, 5, 10, 15);
        break;
    case Icon::Minus:
        line_(5, 10, 15, 10);
        break;
    case Icon::Trash:
        line_(3.5f, 5.5f, 16.5f, 5.5f);
        nvgMoveTo(vg, 5, 5.5f);
        nvgLineTo(vg, 6, 17);
        nvgLineTo(vg, 14, 17);
        nvgLineTo(vg, 15, 5.5f);
        nvgMoveTo(vg, 8, 5.5f);
        nvgLineTo(vg, 8, 3);
        nvgLineTo(vg, 12, 3);
        nvgLineTo(vg, 12, 5.5f);
        break;
    case Icon::Duplicate:
        nvgRoundedRect(vg, 7, 7, 10, 10, 2);
        nvgMoveTo(vg, 13, 4);
        nvgLineTo(vg, 5, 4);
        nvgQuadTo(vg, 4, 4, 4, 5);
        nvgLineTo(vg, 4, 13);
        break;
    case Icon::ToFront:
    case Icon::ToBack: {
        bool front = which == Icon::ToFront;
        nvgRoundedRect(vg, 4, 9, 12, 8, 2);
        if (front) {
            line_(10, 13, 10, 3);
            line_(10, 3, 7, 6);
            line_(10, 3, 13, 6);
        } else {
            line_(10, 3, 10, 13);
            line_(10, 13, 7, 10);
            line_(10, 13, 13, 10);
        }
        break;
    }
    case Icon::Git:
        nvgCircle(vg, 6, 5, 2);
        nvgCircle(vg, 6, 15, 2);
        nvgCircle(vg, 14, 8, 2);
        line_(6, 7, 6, 13);
        nvgMoveTo(vg, 14, 10);
        nvgQuadTo(vg, 14, 13, 7.5f, 14);
        break;
    case Icon::Hachure:
    case Icon::CrossHatch:
    case Icon::Solid: {
        nvgRoundedRect(vg, 3.5f, 3.5f, 13, 13, 2);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgScissor(vg, 3.5f, 3.5f, 13, 13);
        if (which == Icon::Solid) {
            nvgRoundedRect(vg, 3.5f, 3.5f, 13, 13, 2);
            nvgFill(vg);
        } else {
            for (float k = -10; k < 20; k += 4) {
                line_(k, 17, k + 13, 3);
                if (which == Icon::CrossHatch) {
                    line_(k, 3, k + 13, 17);
                }
            }
            nvgStrokeWidth(vg, 1);
        }
        break;
    }
    case Icon::Thin:
    case Icon::Bold:
    case Icon::ExtraBold:
        nvgStrokeWidth(vg, which == Icon::Thin       ? 1.f
                           : which == Icon::Bold ? 2.5f
                                                 : 4.f);
        line_(4, 10, 16, 10);
        break;
    case Icon::Architect:
        nvgMoveTo(vg, 3, 15);
        nvgBezierTo(vg, 8, 15, 12, 5, 17, 5);
        break;
    case Icon::Artist:
        nvgMoveTo(vg, 3, 15);
        nvgBezierTo(vg, 7, 16, 9, 6, 12, 7);
        nvgBezierTo(vg, 14, 8, 15, 5, 17, 5);
        break;
    case Icon::Search:
        nvgCircle(vg, 8.5f, 8.5f, 5.5f);
        line_(12.5f, 12.5f, 16.5f, 16.5f);
        break;
    case Icon::Cartoonist:
        nvgMoveTo(vg, 3, 15);
        nvgBezierTo(vg, 5, 18, 6, 6, 9, 9);
        nvgBezierTo(vg, 11, 12, 12, 3, 14, 7);
        nvgBezierTo(vg, 15, 9, 16, 6, 17, 5);
        break;
    }
    nvgStroke(vg);
    nvgRestore(vg);
}

} // namespace ui
