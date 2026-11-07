#pragma once

#include <algorithm>
#include <cmath>

struct Vec2 {
    double x = 0, y = 0;
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, double k) { return {a.x * k, a.y * k}; }
inline double length(Vec2 a) { return std::hypot(a.x, a.y); }

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;

    double right() const { return x + w; }
    double bottom() const { return y + h; }
    Vec2 center() const { return {x + w / 2, y + h / 2}; }
    bool contains(Vec2 p) const {
        return p.x >= x && p.x <= right() && p.y >= y && p.y <= bottom();
    }
    bool contains(const Rect& r) const {
        return r.x >= x && r.y >= y && r.right() <= right() &&
               r.bottom() <= bottom();
    }
    bool intersects(const Rect& r) const {
        return r.x <= right() && r.right() >= x && r.y <= bottom() &&
               r.bottom() >= y;
    }
    Rect expanded(double d) const { return {x - d, y - d, w + 2 * d, h + 2 * d}; }

    static Rect from_points(Vec2 a, Vec2 b) {
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x),
                std::abs(b.y - a.y)};
    }
};

// Where a node's local space lands on screen:
// local (qx, qy) -> screen (ox + qx * s, oy + qy * s)
// A node is always 1 local unit wide, so `s` is also its width in pixels.
struct Xform {
    double ox = 0, oy = 0, s = 1;

    Vec2 to_screen(Vec2 q) const { return {ox + q.x * s, oy + q.y * s}; }
    Vec2 to_local(Vec2 p) const { return {(p.x - ox) / s, (p.y - oy) / s}; }
};

// The camera's numbers are always in the current anchor node's local units.
struct Camera {
    double x = 0, y = 0, scale = 1; // anchor-local point at screen top-left

    Xform xform() const { return {-x * scale, -y * scale, scale}; }

    // Zoom by `factor`, keeping the point under `screen` fixed.
    void zoom_at(Vec2 screen, double factor) {
        double lx = screen.x / scale + x, ly = screen.y / scale + y;
        scale *= factor;
        x = lx - screen.x / scale;
        y = ly - screen.y / scale;
    }

    void pan(double dx_px, double dy_px) {
        x -= dx_px / scale;
        y -= dy_px / scale;
    }
};
