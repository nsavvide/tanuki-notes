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

inline const uint32_t stroke_colors[] = {0x1e1e1eff, 0xe03131ff, 0x2f9e44ff,
                                         0x1971c2ff, 0xf08c00ff};
inline const uint32_t fill_colors[] = {0x00000000, 0xffc9c9ff, 0xb2f2bbff,
                                       0xa5d8ffff, 0xffec99ff};

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
