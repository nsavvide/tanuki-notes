#pragma once

// Hand-drawn ("sketchy") geometry in the spirit of rough.js, which is what
// gives Excalidraw its look. Everything works in screen pixels and only adds
// path segments: callers do nvgBeginPath() ... nvgStroke()/nvgFill().
//
// All wobble is measured in `unit` (the stroke width in pixels), so a shape
// looks identical at every zoom level, just like in Excalidraw.

#include "geom.hpp"

#include <cstdint>
#include <vector>

struct NVGcontext;

namespace rough {

// Small deterministic PRNG: the same seed gives the same wobble every frame.
struct Rng {
    uint32_t state;
    explicit Rng(uint32_t seed) : state(seed ? seed : 1) {}
    double next() { // [0, 1)
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (state & 0xffffff) / double(0x1000000);
    }
};

struct Opts {
    double unit = 2;   // stroke width in px
    int roughness = 1; // 0 = clean lines
};

void line(NVGcontext* vg, Vec2 a, Vec2 b, Opts o, Rng& rng);
void polygon(NVGcontext* vg, const std::vector<Vec2>& pts, Opts o, Rng& rng);
void ellipse(NVGcontext* vg, Rect r, Opts o, Rng& rng);

// Outline points of shapes, for fills and hit testing.
std::vector<Vec2> ellipse_points(Rect r, int n);
std::vector<Vec2> diamond_points(Rect r);
std::vector<Vec2> rect_points(Rect r);

// Hatch lines inside `poly`, only where they cross `viewport`.
void hachure(NVGcontext* vg, const std::vector<Vec2>& poly, double gap,
             double angle_deg, Rect viewport, Opts o, Rng& rng);

} // namespace rough
