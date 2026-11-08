#pragma once

// Excalidraw-style interface pieces: floating white "islands", icon buttons
// and the color palette. Pure drawing; App owns layout and behavior.

#include "geom.hpp"

#include <nanovg.h>

#include <cstdint>
#include <string>

namespace ui {

// Palette (Excalidraw light theme).
inline NVGcolor rgb(uint32_t hex, unsigned char a = 255) {
    return nvgRGBA((hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff, a);
}
constexpr uint32_t primary = 0x6965db;      // selection, accents
constexpr uint32_t primary_light = 0xe0dfff; // active tool background
constexpr uint32_t hover = 0xf1f0ff;
constexpr uint32_t ink = 0x1b1b1f;  // icons, text
constexpr uint32_t muted = 0x868e96; // secondary text
constexpr uint32_t line = 0xe9ecef;

// Open Color (Excalidraw's palette): 12 hues in rainbow order, each in
// shades 1, 2, 4, 6 and 9 (0xRRGGBB).
constexpr int n_hues = 12, n_shades = 5;
inline const uint32_t hues[n_hues][n_shades] = {
    {0xffe3e3, 0xffc9c9, 0xff8787, 0xfa5252, 0xc92a2a}, // red
    {0xffdeeb, 0xfcc2d7, 0xf783ac, 0xe64980, 0xa61e4d}, // pink
    {0xf3d9fa, 0xeebefa, 0xda77f2, 0xbe4bdb, 0x862e9c}, // grape
    {0xe5dbff, 0xd0bfff, 0x9775fa, 0x7950f2, 0x5f3dc4}, // violet
    {0xdbe4ff, 0xbac8ff, 0x748ffc, 0x4c6ef5, 0x364fc7}, // indigo
    {0xd0ebff, 0xa5d8ff, 0x4dabf7, 0x228be6, 0x1864ab}, // blue
    {0xc5f6fa, 0x99e9f2, 0x3bc9db, 0x15aabf, 0x0b7285}, // cyan
    {0xc3fae8, 0x96f2d7, 0x38d9a9, 0x12b886, 0x087f5b}, // teal
    {0xd3f9d8, 0xb2f2bb, 0x69db7c, 0x40c057, 0x2b8a3e}, // green
    {0xe9fac8, 0xd8f5a2, 0xa9e34b, 0x82c91e, 0x5c940d}, // lime
    {0xfff3bf, 0xffec99, 0xffd43b, 0xfab005, 0xe67700}, // yellow
    {0xffe8cc, 0xffd8a8, 0xffa94d, 0xfd7e14, 0xd9480f}, // orange
};
// Shade as a 0xRRGGBBAA style color.
inline uint32_t shade(int hue, int k) { return (hues[hue][k] << 8) | 0xff; }
// Auto colors: a light fill with a dark outline of the same hue.
constexpr int auto_fill_shade = 1, auto_stroke_shade = 4;
// Order auto colors are handed out in: neighbours get far-apart hues.
inline const int auto_order[n_hues] = {5, 11, 8, 2, 10, 6, 0, 9, 3, 7, 1, 4};
// Palette hue of a 0xRRGGBBAA color, or -1.
inline int hue_of(uint32_t rgba) {
    for (int h = 0; h < n_hues; ++h) {
        for (int k = 0; k < n_shades; ++k) {
            if (shade(h, k) == rgba) {
                return h;
            }
        }
    }
    return -1;
}

enum class Icon {
    Hand, Select, Rect, Diamond, Ellipse, Arrow, Line, Draw, Text, Image,
    Eraser, Undo, Redo, Plus, Minus, Trash, Duplicate, ToFront, ToBack, Git,
    Hachure, CrossHatch, Solid, Thin, Bold, ExtraBold, Architect, Artist,
    Cartoonist, Search,
};

// White rounded panel with a soft shadow.
void island(NVGcontext* vg, Rect r);

void icon(NVGcontext* vg, Icon which, Rect box, NVGcolor c);

// Button background for hover/active states.
void button_bg(NVGcontext* vg, Rect r, bool active, bool hovered);

// Single-line UI text; returns its width.
float label(NVGcontext* vg, const std::string& s, double x, double y,
            float size, NVGcolor c, bool medium = false,
            int align = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

void swatch(NVGcontext* vg, Rect r, uint32_t rgba, bool active);

} // namespace ui
