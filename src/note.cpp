#include "note.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string_view>
#include <unordered_map>

namespace note {

namespace {

enum class Block { Para, H1, H2, H3, Bullet, Number, Todo, Quote, Divider };

struct BlockInfo {
    Block kind = Block::Para;
    size_t indent = 0;     // leading spaces
    size_t content = 0;    // byte where the text after the marker starts
    int number = 0;        // Number
    bool checked = false;  // Todo
};

BlockInfo parse_block(std::string_view s, size_t ls, size_t le) {
    BlockInfo b;
    size_t i = ls;
    while (i < le && s[i] == ' ') {
        ++i;
    }
    b.indent = i - ls;
    b.content = ls;
    std::string_view r = s.substr(i, le - i);
    auto starts = [&](std::string_view p) { return r.starts_with(p); };
    if (starts("### ")) {
        b = {Block::H3, b.indent, i + 4};
    } else if (starts("## ")) {
        b = {Block::H2, b.indent, i + 3};
    } else if (starts("# ")) {
        b = {Block::H1, b.indent, i + 2};
    } else if (r == "---" || r == "***") {
        b = {Block::Divider, b.indent, le};
    } else if (r.size() >= 5 && std::string_view("-*+").find(r[0]) != std::string_view::npos &&
               r[1] == ' ' && r[2] == '[' && (r[3] == ' ' || r[3] == 'x' || r[3] == 'X') &&
               r[4] == ']' && (r.size() == 5 || r[5] == ' ')) {
        b = {Block::Todo, b.indent, std::min(i + 6, le)};
        b.checked = r[3] != ' ';
    } else if (starts("[ ] ") || starts("[x] ") || starts("[X] ")) {
        b = {Block::Todo, b.indent, i + 4};
        b.checked = r[1] != ' ';
    } else if (starts("- ") || starts("* ") || starts("+ ")) {
        b = {Block::Bullet, b.indent, i + 2};
    } else if (starts("> ")) {
        b = {Block::Quote, b.indent, i + 2};
    } else {
        size_t d = 0;
        while (d < r.size() && std::isdigit((unsigned char)r[d])) {
            ++d;
        }
        if (d > 0 && d < 10 && r.substr(d).starts_with(". ")) {
            b = {Block::Number, b.indent, i + d + 2};
            b.number = std::stoi(std::string(r.substr(0, d)));
        }
    }
    return b;
}

struct Span {
    size_t begin, end;
    uint16_t flags;
};

// Inline Markdown in [b, e): markers get the Marker flag.
void parse_inline(std::string_view s, size_t b, size_t e, uint16_t flags,
                  std::vector<Span>& out) {
    size_t plain = b;
    auto flush = [&](size_t upto) {
        if (upto > plain) {
            out.push_back({plain, upto, flags});
        }
    };
    size_t i = b;
    while (i < e) {
        std::string_view rest = s.substr(i, e - i);
        auto pair = [&](std::string_view open, std::string_view close, uint16_t add,
                        bool nested) {
            if (!rest.starts_with(open)) {
                return false;
            }
            size_t c = s.find(close, i + open.size());
            if (c == std::string_view::npos || c + close.size() > e || c == i + open.size()) {
                return false;
            }
            flush(i);
            out.push_back({i, i + open.size(), uint16_t(flags | Marker)});
            if (nested) {
                parse_inline(s, i + open.size(), c, flags | add, out);
            } else {
                out.push_back({i + open.size(), c, uint16_t(flags | add)});
            }
            out.push_back({c, c + close.size(), uint16_t(flags | Marker)});
            i = c + close.size();
            plain = i;
            return true;
        };
        if (pair("`", "`", Code, false) || pair("[[", "]]", Link, false) ||
            pair("**", "**", Bold, true) || pair("~~", "~~", Strike, true) ||
            (!rest.starts_with("**") && pair("*", "*", Italic, true))) {
            continue;
        }
        ++i;
    }
    flush(e);
}

size_t next_cp(const std::string& s, size_t i) {
    ++i;
    while (i < s.size() && (s[i] & 0xc0) == 0x80) {
        ++i;
    }
    return i;
}

double measure(NVGcontext* vg, const std::string& s, size_t b, size_t e, uint16_t flags,
               double size) {
    if (flags & Hidden || e <= b) {
        return 0;
    }
    return text::measure(vg, font(flags), s.substr(b, e - b), size);
}

Layout build(NVGcontext* vg, const std::string& s, double width, int raw_line) {
    Layout L;
    double y = 0;
    size_t ls = 0;
    int li = 0;
    std::vector<int> runs; // numbered-list count per nesting level
    while (true) {
        size_t le = std::min(s.find('\n', ls), s.size());
        bool raw = li == raw_line;
        BlockInfo b = parse_block(s, ls, le);
        double size = b.kind == Block::H1 ? 1.875 : b.kind == Block::H2 ? 1.5
                    : b.kind == Block::H3 ? 1.25 : 1.0;
        bool heading = size > 1;
        double lh = heading ? size * 1.3 : 1.5;
        if (heading && li > 0) {
            y += 0.6 * size; // room above headings
        }
        int level = int(b.indent / 2);
        double list_x = level * 1.5;
        double content_x = b.kind == Block::Bullet || b.kind == Block::Number ? list_x + 1.5
                         : b.kind == Block::Todo ? list_x + 1.7
                         : b.kind == Block::Quote ? 1.0
                                                  : 0;
        // Each nesting level counts on its own; a deeper item doesn't break
        // the count of its parent, anything else at a level restarts it.
        bool listy = b.kind == Block::Bullet || b.kind == Block::Number || b.kind == Block::Todo;
        runs.resize(listy ? size_t(level) + 1 : 0);
        int number_run = 0;
        if (b.kind == Block::Number) {
            number_run = runs[size_t(level)] = runs[size_t(level)] ? runs[size_t(level)] + 1 : b.number;
        } else if (listy) {
            runs[size_t(level)] = 0;
        }

        // Spans of this line: the marker, then the inline-parsed text.
        uint16_t base = heading ? Bold : 0;
        if (b.kind == Block::Todo && b.checked) {
            base |= Done;
        }
        std::vector<Span> spans;
        if (b.kind == Block::Divider) {
            spans.push_back({ls, le, uint16_t(raw ? Marker : Hidden)});
        } else {
            if (b.content > ls) {
                spans.push_back({ls, ls + b.indent, Hidden});
                spans.push_back({ls + b.indent, b.content, uint16_t(raw ? Marker : Hidden)});
            }
            parse_inline(s, b.content, le, base, spans);
        }
        if (!raw) {
            for (auto& sp : spans) {
                if (sp.flags & Marker) {
                    sp.flags = uint16_t((sp.flags & ~Marker) | Hidden);
                }
            }
        }

        // The block marker hangs in the margin on the raw line, so the
        // text after it stays where it is.
        double x = content_x;
        int first_line = int(L.lines.size());
        L.lines.push_back({y, lh});
        size_t si = 0;
        if (raw && b.content > ls && b.kind != Block::Divider) {
            const Span& m = spans[1];
            double mw = measure(vg, s, m.begin, m.end, m.flags, size);
            L.pieces.push_back({spans[0].begin, spans[0].end, content_x - mw, 0,
                                first_line, size, Hidden});
            L.pieces.push_back({m.begin, m.end, content_x - mw, mw, first_line, size, m.flags});
            si = 2;
        }
        auto place = [&](size_t pb, size_t pe, uint16_t f) {
            double w = measure(vg, s, pb, pe, f, size);
            int line = int(L.lines.size()) - 1;
            if (!L.pieces.empty()) {
                Piece& last = L.pieces.back();
                if (last.line == line && last.flags == f && last.end == pb && last.size == size) {
                    last.end = pe; // same style continues: one piece
                    last.w += w;
                    x += w;
                    return;
                }
            }
            L.pieces.push_back({pb, pe, x, w, line, size, f});
            x += w;
        };
        // Words (text up to and including the spaces after it) wrap as a
        // whole; a word may cross style spans ("**bo**ld").
        struct Chunk {
            size_t b, e;
            uint16_t f;
        };
        std::vector<Chunk> word;
        double word_w = 0, word_fit = 0;
        bool line_has_text = false;
        auto flush_word = [&] {
            if (word.empty()) {
                return;
            }
            if (line_has_text && x + word_fit > width) {
                y += lh;
                L.lines.push_back({y, lh});
                x = content_x;
            }
            for (const Chunk& c : word) {
                place(c.b, c.e, c.f);
            }
            line_has_text = true;
            word.clear();
            word_w = word_fit = 0;
        };
        for (; si < spans.size(); ++si) {
            const Span& sp = spans[si];
            size_t i = sp.begin;
            while (i < sp.end) {
                size_t j = i;
                while (j < sp.end && s[j] != ' ') {
                    j = next_cp(s, j);
                }
                size_t k = j;
                while (k < sp.end && s[k] == ' ') {
                    ++k;
                }
                word.push_back({i, k, sp.flags});
                word_fit = word_w + measure(vg, s, i, j, sp.flags, size);
                word_w += measure(vg, s, i, k, sp.flags, size);
                if (k > j) {
                    flush_word(); // the word ended at a space
                }
                i = k;
            }
        }
        flush_word();
        if (L.pieces.empty() || L.pieces.back().line != int(L.lines.size()) - 1 ||
            L.pieces.back().end < le) {
            // Empty line (or only hidden bytes): a zero-width piece holds the caret.
            L.pieces.push_back({le, le, x, 0, int(L.lines.size()) - 1, size, Hidden});
        }
        double block_bottom = y + lh;

        // Decorations, drawn where the hidden markers were.
        if (!raw) {
            double cy = L.lines[size_t(first_line)].y;
            switch (b.kind) {
            case Block::Bullet:
                L.decos.push_back({DecoKind::Bullet, content_x - 0.9, cy, 0, lh, level, "", 0, false});
                break;
            case Block::Number:
                L.decos.push_back({DecoKind::Number, content_x - 0.35, cy, 0, lh, level,
                                   std::format("{}.", number_run), 0, false});
                break;
            case Block::Todo:
                L.decos.push_back({DecoKind::Checkbox, content_x - 1.45, cy + (lh - 0.95) / 2,
                                   0.95, 0.95, level, "", s.find('[', ls), b.checked});
                break;
            case Block::Quote:
                L.decos.push_back({DecoKind::Quote, 0.2, cy + 0.1, 0.16, block_bottom - cy - 0.2,
                                   0, "", 0, false});
                break;
            case Block::Divider:
                L.decos.push_back({DecoKind::Divider, 0, cy + lh / 2, width, 0, 0, "", 0, false});
                break;
            default:
                break;
            }
        }
        y = block_bottom + 0.15; // a little air between blocks
        if (le >= s.size()) {
            break;
        }
        ls = le + 1;
        ++li;
    }
    L.height = std::max(1.5, y - 0.15);
    return L;
}

} // namespace

text::Font font(uint16_t flags) {
    if (flags & Code) {
        return text::Mono;
    }
    if (flags & Bold) {
        return text::UiBold;
    }
    return flags & Italic ? text::UiItalic : text::Ui;
}

std::shared_ptr<const Layout> layout(NVGcontext* vg, const std::string& text,
                                     double width, int raw_line) {
    // ponytail: unbounded until 2000 entries, then cleared; an LRU if it ever matters.
    static std::unordered_map<std::string, std::shared_ptr<const Layout>> cache;
    std::string key = std::format("{:.3f}|{}|", width, raw_line) + text;
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    if (cache.size() > 2000) {
        cache.clear();
    }
    auto l = std::make_shared<const Layout>(build(vg, text, width, raw_line));
    cache.emplace(std::move(key), l);
    return l;
}

int line_of(const std::string& text, size_t pos) {
    pos = std::min(pos, text.size());
    return int(std::count(text.begin(), text.begin() + long(pos), '\n'));
}

Layout::Caret Layout::caret(NVGcontext* vg, const std::string& s, size_t pos) const {
    const Piece* best = nullptr;
    for (const Piece& p : pieces) {
        if (p.begin <= pos && pos <= p.end) {
            best = &p;
            if (pos < p.end) {
                break;
            }
        }
    }
    if (!best) {
        best = &pieces.back();
        pos = best->end;
    }
    const Line& l = lines[size_t(best->line)];
    double x = best->x + measure(vg, s, best->begin, pos, best->flags, best->size);
    return {x, l.y, l.h};
}

size_t Layout::hit(NVGcontext* vg, const std::string& s, double x, double y) const {
    int line = int(lines.size()) - 1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (y < lines[i].y + lines[i].h) {
            line = int(i);
            break;
        }
    }
    const Piece* last = nullptr;
    for (const Piece& p : pieces) {
        if (p.line != line) {
            continue;
        }
        if (p.flags & Hidden) {
            last = last ? last : &p;
            continue;
        }
        if (x < p.x) {
            return p.begin;
        }
        if (x <= p.x + p.w) {
            // The character boundary nearest to x.
            size_t prev = p.begin;
            double prev_x = p.x;
            for (size_t i = p.begin; i < p.end;) {
                size_t n = next_cp(s, i);
                double nx = p.x + measure(vg, s, p.begin, n, p.flags, p.size);
                if (x < (prev_x + nx) / 2) {
                    return prev;
                }
                prev = n;
                prev_x = nx;
                i = n;
            }
            return p.end;
        }
        last = &p;
    }
    return last ? last->end : s.size();
}

ListItem list_item(const std::string& text, size_t ls) {
    size_t le = std::min(text.find('\n', ls), text.size());
    BlockInfo b = parse_block(text, ls, le);
    ListItem it;
    std::string indent(b.indent, ' ');
    switch (b.kind) {
    case Block::Bullet:
        it.next = indent + text.substr(ls + b.indent, 2);
        break;
    case Block::Number:
        it.next = indent + std::format("{}. ", b.number + 1);
        break;
    case Block::Todo:
        it.next = indent + "- [ ] ";
        break;
    case Block::Quote:
        it.next = indent + "> ";
        break;
    default:
        return it;
    }
    it.marker_len = b.content - ls;
    it.indent = b.indent;
    return it;
}

} // namespace note
