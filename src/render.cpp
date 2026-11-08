#include "render.hpp"

#include "code.hpp"
#include "assets.hpp"
#include "rough.hpp"

#include <nanovg.h>

#include <algorithm>
#include <string_view>

static NVGcolor color(uint32_t c, float alpha = 1) {
    return nvgRGBA((c >> 24) & 0xff, (c >> 16) & 0xff, (c >> 8) & 0xff,
                   (unsigned char)((c & 0xff) * alpha));
}

std::vector<Vec2> screen_points(const Node& n, Xform t) {
    std::vector<Vec2> out;
    out.reserve(n.points.size());
    for (Vec2 p : n.points) {
        out.push_back(t.to_screen({p.x, p.y * n.aspect()}));
    }
    return out;
}

static double title_target(const Node& n) {
    return !n.children.empty() || n.kind == Kind::Pdf ? 1 : 0;
}

TextLayout layout_text(NVGcontext* vg, const Node& n, Xform t) {
    TextLayout L;
    Rect r = screen_rect(n, t);
    L.px = n.style.font_size * parent_scale(n, t);
    if (n.kind == Kind::Text) {
        L.lines = text::wrap(vg, text::Hand, n.text, L.px, 0);
        L.top = r.y;
        L.line_x.assign(L.lines.size(), r.x);
        return L;
    }
    // Shape label: centered and wrapped, or a title in the top-left corner
    // once the shape has children of its own. In between while it glides.
    double k = n.title_k < 0 ? title_target(n) : n.title_k;
    k = k * k * (3 - 2 * k); // ease in-out
    double pad = std::max(L.px * 0.5, r.w * 0.04);
    double max_w = k > 0 ? r.w - 2 * pad : r.w * 0.8;
    L.lines = text::wrap(vg, text::Hand, n.text, L.px, max_w);
    double block_h = L.lines.size() * L.px * text::line_height;
    auto lerp = [k](double a, double b) { return a + (b - a) * k; };
    L.top = lerp(r.y + (r.h - block_h) / 2, r.y + pad * 0.6);
    for (const auto& span : L.lines) {
        double w = text::measure(vg, text::Hand,
                                 n.text.substr(span.begin, span.end - span.begin),
                                 L.px);
        L.line_x.push_back(lerp(r.x + (r.w - w) / 2, r.x + pad));
    }
    return L;
}

void fit_text_bounds(NVGcontext* vg, Node& n) {
    // Measure in parent units directly: text width scales linearly.
    double fs = n.style.font_size;
    double w = fs * 0.5;
    int lines = 1;
    size_t start = 0;
    while (true) {
        size_t nl = n.text.find('\n', start);
        w = std::max(w, text::measure(vg, text::Hand,
                                      n.text.substr(start, nl - start), fs));
        if (nl == std::string::npos) {
            break;
        }
        ++lines;
        start = nl + 1;
    }
    n.w = w + fs * 0.1;
    n.h = lines * fs * text::line_height;
}

// Code card layout, in multiples of the font size.
constexpr double code_pad = 0.9, code_top = 1.9, code_lh = 1.45;

static std::string expand_tabs(std::string_view line) {
    std::string out;
    for (char c : line) {
        if (c == '\t') {
            out.append(4 - out.size() % 4, ' ');
        } else {
            out += c;
        }
    }
    return out;
}

static size_t codepoints(std::string_view s) {
    return size_t(std::count_if(s.begin(), s.end(),
                                [](char c) { return (c & 0xc0) != 0x80; }));
}

template <class F> static void for_each_line(const std::string& s, F f) {
    size_t start = 0;
    while (true) {
        size_t nl = s.find('\n', start);
        f(std::string_view(s).substr(start, nl - start));
        if (nl == std::string::npos) {
            return;
        }
        start = nl + 1;
    }
}

void fit_code_bounds(NVGcontext* vg, Node& n) {
    double fs = n.style.font_size;
    double adv = text::measure(vg, text::Mono, "M", fs);
    size_t cols = 24, lines = 0;
    for_each_line(n.text, [&](std::string_view l) {
        cols = std::max(cols, codepoints(expand_tabs(l)) + 1);
        ++lines;
    });
    lines = std::max<size_t>(lines, 3);
    n.w = cols * adv + 2 * code_pad * fs;
    n.h = lines * code_lh * fs + (code_top + code_pad) * fs;
}

static void draw_code(RenderCtx& ctx, const Node& n, Xform t, float alpha) {
    NVGcontext* vg = ctx.vg;
    Rect r = screen_rect(n, t);
    double px = n.style.font_size * parent_scale(n, t);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, float(r.x), float(r.y), float(r.w), float(r.h),
                   float(std::min(px * 0.6, r.w * 0.05)));
    nvgFillColor(vg, color((code::background << 8) | 0xff, alpha));
    nvgFill(vg);
    if (px < 1) {
        return;
    }
    nvgSave(vg);
    nvgIntersectScissor(vg, float(r.x), float(r.y), float(r.w), float(r.h));

    const code::Lang& lang = code::lang(n.lang);
    double small = px * 0.75;
    text::draw_line(vg, text::Mono, lang.name,
                    r.right() - code_pad * px -
                        text::measure(vg, text::Mono, lang.name, small),
                    r.y + 0.5 * px, small,
                    color((code::muted << 8) | 0xff, alpha), ctx.viewport);

    double adv = text::measure(vg, text::Mono, "M", px);
    double lh = code_lh * px;
    double y = r.y + code_top * px;
    bool in_block = false;
    for_each_line(n.text, [&](std::string_view raw) {
        if (y > ctx.viewport.bottom()) {
            return; // ponytail: keeps lexing lines below the screen; cheap enough
        }
        std::string line = expand_tabs(raw);
        auto toks = code::highlight(lang.name, line, in_block);
        if (y + lh >= ctx.viewport.y) {
            double x = r.x + code_pad * px;
            for (const auto& tk : toks) {
                std::string_view s = std::string_view(line).substr(tk.begin, tk.end - tk.begin);
                text::draw_line(vg, text::Mono, std::string(s), x,
                                y + (code_lh - text::line_height) / 2 * px, px,
                                color((code::color(tk.kind) << 8) | 0xff, alpha),
                                ctx.viewport);
                x += codepoints(s) * adv;
            }
        }
        y += lh;
    });
    nvgRestore(vg);
}

CaretBox caret_box(NVGcontext* vg, const Node& n, Xform t, size_t pos) {
    const std::string& s = n.text;
    pos = std::min(pos, s.size());
    if (n.kind == Kind::Code) {
        Rect r = screen_rect(n, t);
        double px = n.style.font_size * parent_scale(n, t);
        size_t lb = pos == 0 ? std::string::npos : s.rfind('\n', pos - 1);
        lb = lb == std::string::npos ? 0 : lb + 1;
        size_t line = size_t(std::count(s.begin(), s.begin() + long(lb), '\n'));
        double adv = text::measure(vg, text::Mono, "M", px);
        size_t col = codepoints(expand_tabs(std::string_view(s).substr(lb, pos - lb)));
        return {r.x + code_pad * px + col * adv, r.y + code_top * px + line * code_lh * px,
                code_lh * px};
    }
    TextLayout L = layout_text(vg, n, t);
    size_t line = 0;
    for (size_t i = 0; i < L.lines.size(); ++i) {
        if (L.lines[i].begin <= pos) {
            line = i;
        }
    }
    if (L.lines.empty()) {
        return {L.line_x.empty() ? screen_rect(n, t).center().x : L.line_x[0], L.top,
                L.px * text::line_height};
    }
    const auto& span = L.lines[line];
    size_t end = std::min(pos, span.end);
    double x = L.line_x[line] +
               text::measure(vg, text::Hand, s.substr(span.begin, end - span.begin), L.px);
    return {x, L.top + line * L.px * text::line_height, L.px * text::line_height};
}

std::vector<text::Span> screen_lines(NVGcontext* vg, const Node& n, Xform t) {
    if (n.kind != Kind::Code) {
        return layout_text(vg, n, t).lines;
    }
    std::vector<text::Span> out;
    size_t start = 0;
    for_each_line(n.text, [&](std::string_view l) {
        out.push_back({start, start + l.size()});
        start += l.size() + 1;
    });
    return out;
}

static void draw_label(RenderCtx& ctx, const Node& n, Xform t, float alpha) {
    if (n.kind == Kind::Code) {
        return; // its text is the code itself
    }
    double target = title_target(n);
    if (n.title_k < 0) {
        n.title_k = target;
    } else if (n.title_k != target) {
        double step = ctx.dt / 0.25; // full glide in a quarter second
        n.title_k = target > n.title_k ? std::min(target, n.title_k + step)
                                       : std::max(target, n.title_k - step);
        ctx.animating = true;
    }
    if (n.text.empty()) {
        return;
    }
    TextLayout L = layout_text(ctx.vg, n, t);
    NVGcolor c = color(n.style.stroke, alpha);
    for (size_t i = 0; i < L.lines.size(); ++i) {
        const auto& span = L.lines[i];
        text::draw_line(ctx.vg, text::Hand,
                        n.text.substr(span.begin, span.end - span.begin),
                        L.line_x[i], L.top + i * L.px * text::line_height, L.px,
                        c, ctx.viewport);
    }
}

static void fill_shape(RenderCtx& ctx, const Node& n,
                       const std::vector<Vec2>& poly, double sw, float alpha) {
    if ((n.style.fill & 0xff) == 0) {
        return;
    }
    NVGcontext* vg = ctx.vg;
    NVGcolor fc = color(n.style.fill, alpha);
    if (n.style.fill_style == FillStyle::Solid) {
        nvgBeginPath(vg);
        for (size_t i = 0; i < poly.size(); ++i) {
            if (i == 0) {
                nvgMoveTo(vg, float(poly[i].x), float(poly[i].y));
            } else {
                nvgLineTo(vg, float(poly[i].x), float(poly[i].y));
            }
        }
        nvgClosePath(vg);
        nvgFillColor(vg, fc);
        nvgFill(vg);
        return;
    }
    rough::Rng rng(n.seed * 7 + 3);
    rough::Opts o{sw / 2, n.style.roughness};
    nvgBeginPath(vg);
    rough::hachure(vg, poly, sw * 4, -41, ctx.viewport, o, rng);
    if (n.style.fill_style == FillStyle::CrossHatch) {
        rough::hachure(vg, poly, sw * 4, 49, ctx.viewport, o, rng);
    }
    nvgStrokeColor(vg, fc);
    nvgStrokeWidth(vg, float(std::max(sw / 2, 0.5)));
    nvgStroke(vg);
}

static void stroke_style(NVGcontext* vg, const Node& n, double sw, float alpha) {
    nvgStrokeColor(vg, color(n.style.stroke, alpha));
    nvgStrokeWidth(vg, float(std::max(sw, 0.5)));
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
}

static void draw_shape(RenderCtx& ctx, const Node& n, Xform t, float alpha) {
    NVGcontext* vg = ctx.vg;
    Rect r = screen_rect(n, t);
    double sw = n.style.stroke_width * parent_scale(n, t);
    rough::Rng rng(n.seed);
    rough::Opts o{sw, n.style.roughness};

    switch (n.kind) {
    case Kind::Rect: {
        auto pts = rough::rect_points(r);
        fill_shape(ctx, n, pts, sw, alpha);
        if (sw > 0) {
            nvgBeginPath(vg);
            rough::polygon(vg, pts, o, rng);
            stroke_style(vg, n, sw, alpha);
            nvgStroke(vg);
        }
        break;
    }
    case Kind::Diamond: {
        auto pts = rough::diamond_points(r);
        fill_shape(ctx, n, pts, sw, alpha);
        if (sw > 0) {
            nvgBeginPath(vg);
            rough::polygon(vg, pts, o, rng);
            stroke_style(vg, n, sw, alpha);
            nvgStroke(vg);
        }
        break;
    }
    case Kind::Ellipse: {
        int segs = std::clamp(int((r.w + r.h) / 4), 24, 4096);
        fill_shape(ctx, n, rough::ellipse_points(r, segs), sw, alpha);
        if (sw > 0) {
            nvgBeginPath(vg);
            rough::ellipse(vg, r, o, rng);
            stroke_style(vg, n, sw, alpha);
            nvgStroke(vg);
        }
        break;
    }
    case Kind::Line:
    case Kind::Arrow: {
        auto pts = screen_points(n, t);
        if (pts.size() < 2) {
            break;
        }
        nvgBeginPath(vg);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            rough::line(vg, pts[i], pts[i + 1], o, rng);
        }
        if (n.kind == Kind::Arrow) {
            Vec2 tip = pts.back(), from = pts[pts.size() - 2];
            Vec2 d = tip - from;
            double len = length(d);
            if (len > 0) {
                double head = std::min(len * 0.4, std::max(sw * 10, 8.0));
                double ang = std::atan2(d.y, d.x);
                for (double side : {-0.5, 0.5}) {
                    Vec2 end{tip.x - head * std::cos(ang + side),
                             tip.y - head * std::sin(ang + side)};
                    rough::line(vg, end, tip, o, rng);
                }
            }
        }
        stroke_style(vg, n, sw, alpha);
        nvgStroke(vg);
        break;
    }
    case Kind::Draw: {
        auto pts = screen_points(n, t);
        if (pts.empty()) {
            break;
        }
        nvgBeginPath(vg);
        nvgMoveTo(vg, float(pts[0].x), float(pts[0].y));
        if (pts.size() == 1) {
            nvgLineTo(vg, float(pts[0].x + 0.01), float(pts[0].y));
        }
        // Quadratic curves through midpoints: smooth, no corners.
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            Vec2 mid = (pts[i] + pts[i + 1]) * 0.5;
            nvgQuadTo(vg, float(pts[i].x), float(pts[i].y), float(mid.x),
                      float(mid.y));
        }
        if (pts.size() > 1) {
            nvgLineTo(vg, float(pts.back().x), float(pts.back().y));
        }
        stroke_style(vg, n, sw, alpha);
        nvgStroke(vg);
        break;
    }
    case Kind::Image: {
        const Texture* tex = ctx.assets.image(n.asset);
        nvgBeginPath(vg);
        nvgRect(vg, float(r.x), float(r.y), float(r.w), float(r.h));
        if (tex) {
            nvgFillPaint(vg, nvgImagePattern(vg, float(r.x), float(r.y),
                                             float(r.w), float(r.h), 0,
                                             tex->handle, alpha));
        } else {
            nvgFillColor(vg, nvgRGBA(233, 236, 239, (unsigned char)(255 * alpha)));
        }
        nvgFill(vg);
        break;
    }
    case Kind::Pdf: {
        float rad = float(std::min(r.w, r.h) * 0.015);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, float(r.x), float(r.y), float(r.w), float(r.h), rad);
        nvgFillColor(vg, nvgRGBA(241, 243, 245, (unsigned char)(255 * alpha)));
        nvgFill(vg);
        nvgStrokeColor(vg, nvgRGBA(222, 226, 230, (unsigned char)(255 * alpha)));
        nvgStrokeWidth(vg, 1);
        nvgStroke(vg);
        break;
    }
    case Kind::PdfPage: {
        nvgBeginPath(vg);
        nvgRect(vg, float(r.x), float(r.y), float(r.w), float(r.h));
        nvgFillColor(vg, nvgRGBA(255, 255, 255, (unsigned char)(255 * alpha)));
        nvgFill(vg);
        const Texture* tex =
            ctx.assets.pdf_page(n.asset, n.page, r.w * ctx.pixel_ratio);
        if (tex) {
            nvgFillPaint(vg, nvgImagePattern(vg, float(r.x), float(r.y),
                                             float(r.w), float(r.h), 0,
                                             tex->handle, alpha));
            nvgFill(vg);
        }
        nvgStrokeColor(vg, nvgRGBA(206, 212, 218, (unsigned char)(255 * alpha)));
        nvgStrokeWidth(vg, 1);
        nvgStroke(vg);
        break;
    }
    case Kind::Code:
        draw_code(ctx, n, t, alpha);
        break;
    case Kind::Text:
    case Kind::Root:
        break;
    }
}

void draw_subtree(RenderCtx& ctx, const Node& n, Xform t) {
    Rect r = screen_rect(n, t);
    double sw = n.style.stroke_width * parent_scale(n, t);

    // Culling: children sit inside (and are clipped to) their parent, so an
    // invisible node hides its whole subtree.
    if (n.kind != Kind::Root &&
        (!ctx.viewport.intersects(r.expanded(sw + 2)) ||
         (r.w < 1 && r.h < 1))) {
        return;
    }
    float alpha = ctx.faded.contains(&n) ? 0.3f : 1.0f;

    if (n.kind != Kind::Root && r.w < 3 && r.h < 3) {
        // A dot: shape detail is invisible at this size anyway.
        nvgBeginPath(ctx.vg);
        nvgRect(ctx.vg, float(r.x), float(r.y), float(std::max(r.w, 1.0)),
                float(std::max(r.h, 1.0)));
        nvgFillColor(ctx.vg, color(n.style.stroke, 0.5f * alpha));
        nvgFill(ctx.vg);
        return;
    }

    draw_shape(ctx, n, t, alpha);

    if (!n.children.empty()) {
        nvgSave(ctx.vg);
        if (n.kind != Kind::Root) {
            nvgIntersectScissor(ctx.vg, float(r.x), float(r.y), float(r.w),
                                float(r.h));
        }
        for (const auto& c : n.children) {
            draw_subtree(ctx, *c, child_xform(t, *c));
        }
        nvgRestore(ctx.vg);
    }
    draw_label(ctx, n, t, alpha);
}
