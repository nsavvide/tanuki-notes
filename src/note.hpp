#pragma once

// Text notes are Markdown laid out like a Notion page: Inter, a fixed page
// width with word wrap, headings (# ## ###), bullets (- * +), numbered
// lists (1.), checkboxes (- [ ] / - [x]), quotes (>), dividers (---), and
// **bold**, *italic*, `code`, ~~strike~~ and [[links]] inline.
//
// Markers disappear when rendered, except on the "raw" line (the one with
// the caret), where they show faintly in the margin so the text never moves.
// Layout is in em (multiples of the note's font size), so it is computed
// once per text and width and scaled at any zoom.

#include "text.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct NVGcontext;

namespace note {

enum Flag : uint16_t {
    Bold = 1,
    Italic = 2,
    Code = 4,
    Strike = 8,
    Link = 16,
    Marker = 32, // Markdown syntax, drawn faintly
    Hidden = 64, // Markdown syntax that isn't drawn at all
    Done = 128,  // text of a checked checkbox
};

struct Piece {         // a run of text in one style on one visual line
    size_t begin, end; // bytes of the note's text
    double x, w;       // em from the note's left edge
    int line;          // index into Layout::lines
    double size;       // font size in em
    uint16_t flags;
};

struct Line {
    double y, h; // em from the note's top
};

enum class DecoKind { Bullet, Number, Checkbox, Quote, Divider };

struct Deco {
    DecoKind kind;
    double x, y, w, h; // em
    int level;          // list nesting
    std::string label;  // "3." for numbered items
    size_t pos;         // Checkbox: byte of its "[" (for toggling)
    bool checked;
};

struct Layout {
    std::vector<Piece> pieces;
    std::vector<Line> lines;
    std::vector<Deco> decos;
    double height = 1.5; // em

    struct Caret {
        double x, y, h; // em
    };
    Caret caret(NVGcontext* vg, const std::string& text, size_t pos) const;
    // Byte nearest to a point (em).
    size_t hit(NVGcontext* vg, const std::string& text, double x, double y) const;
};

constexpr double default_width = 26; // em, for new notes

// Cached. `width` is the wrap width in em; `raw_line` is the source line
// shown with its markers (-1: none).
std::shared_ptr<const Layout> layout(NVGcontext* vg, const std::string& text,
                                     double width, int raw_line);

text::Font font(uint16_t flags);

// Source line (0-based) that byte `pos` is on.
int line_of(const std::string& text, size_t pos);

// List item on the line starting at `line_start`: how many bytes its marker
// takes (with indentation; 0 if it isn't one) and the marker that continues
// the list on a new line.
struct ListItem {
    size_t marker_len = 0;
    size_t indent = 0;
    std::string next;
};
ListItem list_item(const std::string& text, size_t line_start);

} // namespace note
