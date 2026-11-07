#include "rough.hpp"

#include <nanovg.h>

#include <numbers>

namespace rough {

// One wobbly bezier from a to b (rough.js `_line`).
static void one_line(NVGcontext* vg, Vec2 a, Vec2 b, Opts o, Rng& rng,
                     bool overlay) {
    double len = length(b - a);
    double len_units = len / std::max(o.unit, 1e-9);
    double gain = len_units < 100   ? 1.0
                  : len_units > 250 ? 0.4
                                    : 1.0 - (len_units - 100) / 150 * 0.6;
    double off = std::min(len / 10, o.roughness * o.unit);
    if (overlay) {
        off /= 2;
    }
    auto r = [&](double m) { return (rng.next() * 2 - 1) * m * gain; };

    double mdx = r((b.y - a.y) / 100 * o.roughness);
    double mdy = r((a.x - b.x) / 100 * o.roughness);
    double diverge = 0.2 + rng.next() * 0.2;
    Vec2 d = b - a;

    nvgMoveTo(vg, float(a.x + r(off)), float(a.y + r(off)));
    nvgBezierTo(vg, float(mdx + a.x + d.x * diverge + r(off)),
                float(mdy + a.y + d.y * diverge + r(off)),
                float(mdx + a.x + 2 * d.x * diverge + r(off)),
                float(mdy + a.y + 2 * d.y * diverge + r(off)),
                float(b.x + r(off)), float(b.y + r(off)));
}

void line(NVGcontext* vg, Vec2 a, Vec2 b, Opts o, Rng& rng) {
    if (o.roughness == 0) {
        nvgMoveTo(vg, float(a.x), float(a.y));
        nvgLineTo(vg, float(b.x), float(b.y));
        return;
    }
    one_line(vg, a, b, o, rng, false);
    one_line(vg, a, b, o, rng, true);
}

void polygon(NVGcontext* vg, const std::vector<Vec2>& pts, Opts o, Rng& rng) {
    for (size_t i = 0; i < pts.size(); ++i) {
        line(vg, pts[i], pts[(i + 1) % pts.size()], o, rng);
    }
}

static int segments_for(Rect r) {
    double perimeter = std::numbers::pi * (r.w + r.h) / 2;
    return std::clamp(int(perimeter / 6), 24, 4096);
}

std::vector<Vec2> ellipse_points(Rect r, int n) {
    std::vector<Vec2> pts(n);
    Vec2 c = r.center();
    for (int i = 0; i < n; ++i) {
        double t = 2 * std::numbers::pi * i / n;
        pts[i] = {c.x + r.w / 2 * std::cos(t), c.y + r.h / 2 * std::sin(t)};
    }
    return pts;
}

std::vector<Vec2> diamond_points(Rect r) {
    Vec2 c = r.center();
    return {{c.x, r.y}, {r.right(), c.y}, {c.x, r.bottom()}, {r.x, c.y}};
}

std::vector<Vec2> rect_points(Rect r) {
    return {{r.x, r.y}, {r.right(), r.y}, {r.right(), r.bottom()},
            {r.x, r.bottom()}};
}

void ellipse(NVGcontext* vg, Rect r, Opts o, Rng& rng) {
    Vec2 c = r.center();
    double rx = r.w / 2, ry = r.h / 2;
    if (o.roughness == 0) {
        nvgEllipse(vg, float(c.x), float(c.y), float(rx), float(ry));
        return;
    }
    int n = segments_for(r);
    double min_r = std::max(std::min(rx, ry), 1e-9);
    for (int pass = 0; pass < 2; ++pass) {
        // Low-frequency radius noise + a start/end overlap, like a pen
        // stroke that doesn't quite close.
        double amp = std::min(0.06, o.roughness * o.unit / min_r);
        double p1 = rng.next() * 6.283, p2 = rng.next() * 6.283;
        double start = rng.next() * 6.283;
        double sweep = 2 * std::numbers::pi + 0.25 + rng.next() * 0.25;
        int steps = int(n * sweep / (2 * std::numbers::pi));
        for (int i = 0; i <= steps; ++i) {
            double t = start + sweep * i / steps;
            double k = 1 + amp * (0.6 * std::sin(2 * t + p1) +
                                  0.4 * std::sin(3 * t + p2));
            float px = float(c.x + rx * k * std::cos(t));
            float py = float(c.y + ry * k * std::sin(t));
            if (i == 0) {
                nvgMoveTo(vg, px, py);
            } else {
                nvgLineTo(vg, px, py);
            }
        }
    }
}

void hachure(NVGcontext* vg, const std::vector<Vec2>& poly, double gap,
             double angle_deg, Rect viewport, Opts o, Rng& rng) {
    if (poly.size() < 3) {
        return;
    }
    // Rotate so hatch lines become horizontal scanlines.
    double a = angle_deg * std::numbers::pi / 180;
    double ca = std::cos(a), sa = std::sin(a);
    auto rot = [&](Vec2 p) { return Vec2{p.x * ca - p.y * sa, p.x * sa + p.y * ca}; };
    auto unrot = [&](Vec2 p) { return Vec2{p.x * ca + p.y * sa, -p.x * sa + p.y * ca}; };

    std::vector<Vec2> rp(poly.size());
    double ymin = 1e300, ymax = -1e300;
    for (size_t i = 0; i < poly.size(); ++i) {
        rp[i] = rot(poly[i]);
        ymin = std::min(ymin, rp[i].y);
        ymax = std::max(ymax, rp[i].y);
    }
    double vymin = 1e300, vymax = -1e300, vxmin = 1e300, vxmax = -1e300;
    for (Vec2 p : rect_points(viewport)) {
        Vec2 q = rot(p);
        vymin = std::min(vymin, q.y);
        vymax = std::max(vymax, q.y);
        vxmin = std::min(vxmin, q.x);
        vxmax = std::max(vxmax, q.x);
    }
    ymin = std::max(ymin, vymin);
    ymax = std::min(ymax, vymax);
    if (ymin >= ymax) {
        return;
    }
    // ponytail: cap at 1500 lines per shape; very dense hatching is noise.
    gap = std::max(gap, (ymax - ymin) / 1500);

    Opts lo = o;
    lo.roughness = std::min(o.roughness, 1);
    std::vector<double> xs;
    for (double y = std::ceil(ymin / gap) * gap; y < ymax; y += gap) {
        xs.clear();
        for (size_t i = 0; i < rp.size(); ++i) {
            Vec2 p = rp[i], q = rp[(i + 1) % rp.size()];
            if ((p.y <= y) != (q.y <= y)) {
                xs.push_back(p.x + (y - p.y) / (q.y - p.y) * (q.x - p.x));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            double x0 = std::max(xs[i], vxmin), x1 = std::min(xs[i + 1], vxmax);
            if (x1 > x0) {
                one_line(vg, unrot({x0, y}), unrot({x1, y}), lo, rng, true);
            }
        }
    }
}

} // namespace rough
