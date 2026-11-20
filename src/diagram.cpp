#include "diagram.hpp"

#include "code.hpp"
#include "render.hpp"
#include "text.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace diagram {

namespace {

// Layout units: roughly screen pixels when a level fills the window.
constexpr double label_fs = 20, title_fs = 22, note_fs = 16, code_fs = 14, edge_fs = 16;
constexpr double pad = 48, gap = 56, stroke = 2;

struct Error {
    int line;
    std::string msg;
};

struct Item {
    std::string id, label, note;
    Kind kind = Kind::Rect;
    int hue = -1; // palette hue; -2 gray, -3 white, -4 no fill
    int dash = 0;
    FillStyle fill = FillStyle::Solid;
    int rough = 1;
    double weight = 1, big = 1;
    std::string layout; // right, down, grid or "" (right with arrows, else grid)
    std::vector<std::string> tags;
    int line = 0;
    Item* parent = nullptr;
    std::vector<Item*> kids;

    // Layout (in the parent's layout units until build_node normalizes).
    double x = 0, y = 0, w = 0, h = 0;
    double W = 1, H = 1; // own content size, in own layout units
    std::vector<std::unique_ptr<Node>> notes; // laid out in own layout units
    Node* node = nullptr;

    bool container() const { return !kids.empty() || !note.empty(); }
};

struct Edge {
    std::string from, to, label;
    Kind kind = Kind::Arrow;
    bool both = false;
    int dash = 0, hue = -1;
    double weight = 1;
    std::vector<std::string> tags;
    int line = 0;
    Item *a = nullptr, *b = nullptr, *lca = nullptr;
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool valid_id(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
    });
}

const std::map<std::string, int> hue_names = {
    {"red", 0},    {"pink", 1},   {"grape", 2}, {"purple", 2}, {"violet", 3},
    {"indigo", 4}, {"blue", 5},   {"cyan", 6},  {"teal", 7},   {"green", 8},
    {"lime", 9},   {"yellow", 10}, {"orange", 11}, {"gray", -2}, {"grey", -2},
    {"white", -3}, {"none", -4},
};

struct Attrs {
    std::optional<Kind> kind;
    int hue = -1, dash = 0, rough = 1;
    std::optional<FillStyle> fill;
    double weight = 1, big = 1;
    std::string layout;
    std::vector<std::string> tags;
};

Attrs parse_attrs(const std::string& s, int line) {
    Attrs a;
    std::string spaced = s;
    std::replace(spaced.begin(), spaced.end(), ',', ' ');
    std::istringstream in(spaced);
    for (std::string t; in >> t;) {
        {
            if (t[0] == '#' && valid_id(t.substr(1))) {
                a.tags.push_back(t.substr(1));
            } else if (auto it = hue_names.find(t); it != hue_names.end()) {
                a.hue = it->second;
            } else if (t == "box" || t == "rect") {
                a.kind = Kind::Rect;
            } else if (t == "ellipse" || t == "circle" || t == "oval") {
                a.kind = Kind::Ellipse;
            } else if (t == "diamond") {
                a.kind = Kind::Diamond;
            } else if (t == "dashed") {
                a.dash = 1;
            } else if (t == "dotted") {
                a.dash = 2;
            } else if (t == "hatch") {
                a.fill = FillStyle::Hachure;
            } else if (t == "crosshatch") {
                a.fill = FillStyle::CrossHatch;
            } else if (t == "clean") {
                a.rough = 0;
            } else if (t == "sketchy") {
                a.rough = 2;
            } else if (t == "bold") {
                a.weight = 2;
            } else if (t == "thin") {
                a.weight = 0.5;
            } else if (t == "big") {
                a.big = 1.6;
            } else if (t == "right" || t == "down" || t == "grid") {
                a.layout = t;
            } else {
                throw Error{line, "unknown attribute '" + t + "'"};
            }
        }
    }
    return a;
}

uint32_t fill_of(int hue) {
    return hue >= 0 ? ui::shade(hue, ui::auto_fill_shade)
           : hue == -2 ? 0xf1f3f5ff
           : hue == -3 ? 0xffffffff
                       : 0x00000000;
}
uint32_t stroke_of(int hue) {
    return hue >= 0 ? ui::shade(hue, ui::auto_stroke_shade) : 0x495057ff;
}

// "\n" in a label is a line break.
std::string unescape(std::string s) {
    for (size_t p; (p = s.find("\\n")) != std::string::npos;) {
        s.replace(p, 2, "\n");
    }
    return s;
}

struct Parsed {
    std::deque<Item> items; // stable addresses
    std::map<std::string, Item*> by_id;
    std::vector<Edge> edges;
    std::vector<Concept> concepts;
    std::map<std::string, int> concept_line;
};

void parse(const std::string& src, Parsed& P) {
    Item& frame = P.items.emplace_back();
    struct Level {
        int indent;
        Item* item;
        Concept* cpt;
    };
    std::vector<Level> stack{{-1, &frame, nullptr}};
    std::string* last_note = nullptr;
    std::map<std::string, int> step_count;

    std::istringstream in(src);
    std::string raw;
    for (int line = 1; std::getline(in, raw); ++line) {
        int indent = 0;
        for (char c : raw) {
            if (c == ' ') {
                ++indent;
            } else if (c == '\t') {
                indent += 4;
            } else {
                break;
            }
        }
        std::string t = trim(raw);
        if (t.empty()) {
            if (last_note) {
                *last_note += "\n";
            }
            continue;
        }
        if (t.starts_with("//")) {
            continue;
        }
        while (stack.back().indent >= indent) {
            stack.pop_back();
        }
        Level& ctx = stack.back();

        if (t == ">" || t.starts_with("> ")) {
            // Notes keep their own indentation after "> " (code blocks).
            size_t at = raw.find('>');
            std::string body = raw.size() > at + 2 ? raw.substr(at + 2) : "";
            last_note = ctx.cpt ? &ctx.cpt->note : &ctx.item->note;
            *last_note += body + "\n";
            continue;
        }
        last_note = nullptr;
        if (t.starts_with("# ") && indent == 0) {
            frame.label = trim(t.substr(2));
            continue;
        }
        if (t.starts_with("layout ")) {
            std::string l = trim(t.substr(7));
            if (l != "right" && l != "down" && l != "grid") {
                throw Error{line, "layout is right, down or grid"};
            }
            ctx.item->layout = l;
            continue;
        }

        // Trailing [attrs] (but not a [[link]]).
        std::string attrs;
        if (t.back() == ']' && !t.ends_with("]]")) {
            size_t open = t.rfind('[');
            if (open == std::string::npos) {
                throw Error{line, "']' without '['"};
            }
            attrs = t.substr(open + 1, t.size() - open - 2);
            t = trim(t.substr(0, open));
        }
        size_t colon = t.find(':');
        std::string head = trim(t.substr(0, colon));
        std::string label = colon == std::string::npos ? "" : unescape(trim(t.substr(colon + 1)));

        if (head.starts_with("concept ")) {
            std::string id = trim(head.substr(8));
            if (!valid_id(id)) {
                throw Error{line, "concept needs an id: concept order-flow: Order flow"};
            }
            if (P.concept_line.contains(id)) {
                throw Error{line, "concept '" + id + "' is defined twice"};
            }
            Attrs a = parse_attrs(attrs, line);
            int hue = a.hue >= 0 ? a.hue : ui::auto_order[P.concepts.size() % ui::n_hues];
            P.concepts.push_back({id, label.empty() ? id : label, "", ui::shade(hue, 3)});
            P.concept_line[id] = line;
            stack.push_back({indent, ctx.item, &P.concepts.back()});
            continue;
        }

        std::vector<std::string> toks;
        {
            std::istringstream hs(head);
            for (std::string w; hs >> w;) {
                toks.push_back(w);
            }
        }
        static const std::set<std::string> ops = {"->", "-->", "..>", "<->", "--", ".."};
        bool is_edge = std::any_of(toks.begin(), toks.end(),
                                   [](auto& w) { return ops.contains(w); });
        if (is_edge) {
            if (toks.size() < 3 || toks.size() % 2 == 0) {
                throw Error{line, "an arrow is: a -> b: label [attrs]"};
            }
            Attrs a = parse_attrs(attrs, line);
            for (size_t i = 1; i + 1 < toks.size(); i += 2) {
                const std::string& op = toks[i];
                if (!ops.contains(op) || !valid_id(toks[i - 1]) || !valid_id(toks[i + 1])) {
                    throw Error{line, "an arrow is: a -> b (with spaces around ->)"};
                }
                Edge e;
                e.from = toks[i - 1];
                e.to = toks[i + 1];
                e.label = label;
                e.kind = op == "--" || op == ".." ? Kind::Line : Kind::Arrow;
                e.both = op == "<->";
                e.dash = a.dash ? a.dash : op == "-->" ? 1 : op.starts_with("..") ? 2 : 0;
                e.hue = a.hue;
                e.weight = a.weight;
                e.tags = a.tags;
                e.line = line;
                if (ctx.cpt) { // a numbered step of the flow
                    int n = ++step_count[ctx.cpt->id];
                    e.label = label.empty() ? std::to_string(n) : std::format("{}. {}", n, label);
                    e.tags.push_back(ctx.cpt->id);
                }
                P.edges.push_back(std::move(e));
            }
            continue;
        }

        if (toks.size() != 1 || !valid_id(toks[0])) {
            throw Error{line, "expected 'id: Label [attrs]', 'a -> b: label', "
                              "'> note', 'concept id: Name' or 'layout right|down|grid'"};
        }
        if (ctx.cpt) {
            throw Error{line, "boxes can't go inside a concept (only > notes and arrows)"};
        }
        if (P.by_id.contains(toks[0])) {
            throw Error{line, "'" + toks[0] + "' is defined twice (ids are unique per file)"};
        }
        Attrs a = parse_attrs(attrs, line);
        Item& it = P.items.emplace_back();
        it.id = toks[0];
        it.label = label.empty() ? toks[0] : label;
        it.kind = a.kind.value_or(Kind::Rect);
        it.hue = a.hue;
        it.dash = a.dash;
        it.fill = a.fill.value_or(FillStyle::Solid);
        it.rough = a.rough;
        it.weight = a.weight;
        it.big = a.big;
        it.layout = a.layout;
        it.tags = a.tags;
        it.line = line;
        it.parent = ctx.item;
        ctx.item->kids.push_back(&it);
        P.by_id[it.id] = &it;
        stack.push_back({indent, &it, nullptr});
    }
}

std::vector<Item*> path_of(Item* it) {
    std::vector<Item*> p;
    for (; it; it = it->parent) {
        p.insert(p.begin(), it);
    }
    return p;
}

// Notes become Markdown notes and ``` fences become code cards.
void make_notes(NVGcontext* vg, Item& it) {
    std::string text, lang;
    bool in_code = false;
    auto flush = [&] {
        while (!text.empty() && text.back() == '\n') {
            text.pop_back();
        }
        if (text.find_first_not_of(" \n") == std::string::npos) {
            text.clear();
            return;
        }
        auto n = make_node(in_code ? Kind::Code : Kind::Text);
        n->text = text;
        if (in_code) {
            n->lang = code::from_fence(lang);
            n->style.font_size = code_fs;
            fit_code_bounds(vg, *n);
        } else {
            n->style.font_size = note_fs;
            fit_text_bounds(vg, *n);
        }
        it.notes.push_back(std::move(n));
        text.clear();
    };
    std::istringstream in(it.note);
    for (std::string l; std::getline(in, l);) {
        if (trim(l).starts_with("```")) {
            flush();
            lang = trim(trim(l).substr(3));
            in_code = !in_code;
            continue;
        }
        text += l + "\n";
    }
    flush();
}

double measure_label(NVGcontext* vg, const std::string& s, double fs, int* lines = nullptr) {
    double w = 0;
    int n = 0;
    std::istringstream in(s);
    for (std::string l; std::getline(in, l); ++n) {
        w = std::max(w, double(text::measure(vg, text::Hand, l, fs)));
    }
    if (lines) {
        *lines = std::max(n, 1);
    }
    return w;
}

// Box size of a kid in its parent's layout units, plus room above it for a
// container's title.
struct Slot {
    double w, h, top;
};

Slot slot_of(NVGcontext* vg, const Item& k) {
    int lines;
    double tw = measure_label(vg, k.label, label_fs, &lines);
    if (k.container()) {
        // Containers are bigger than leaves, more so the more they hold; their
        // shape follows their content.
        double a = k.H / k.W;
        double area = 26000 * (k.kids.size() + k.notes.size() + 2) * k.big * k.big;
        double w = std::max(std::sqrt(area / a), measure_label(vg, k.label, title_fs) + 20);
        int tl;
        measure_label(vg, k.label, title_fs, &tl);
        return {w, w * a, tl * title_fs * text::line_height + 6};
    }
    double w = std::max(150.0, tw + 56), h = std::max(76.0, lines * label_fs * 1.25 + 44);
    if (k.kind == Kind::Ellipse) {
        w *= 1.3;
        h *= 1.35;
    } else if (k.kind == Kind::Diamond) {
        w *= 1.6;
        h *= 1.6;
    }
    return {w * k.big, h * k.big, 0};
}

void layout(NVGcontext* vg, Item& c, const std::vector<Edge>& edges) {
    for (Item* k : c.kids) {
        if (k->container()) {
            layout(vg, *k, edges);
        }
    }
    make_notes(vg, c);

    size_t n = c.kids.size();
    std::map<Item*, size_t> index;
    for (size_t i = 0; i < n; ++i) {
        index[c.kids[i]] = i;
    }
    std::vector<Slot> slots;
    for (Item* k : c.kids) {
        slots.push_back(slot_of(vg, *k));
    }

    // Arrows between kids (or between things inside them) at this level.
    std::set<std::pair<size_t, size_t>> links;
    double label_w = 0;
    for (const Edge& e : edges) {
        if (e.lca != &c) {
            continue;
        }
        auto pa = path_of(e.a), pb = path_of(e.b);
        size_t d = path_of(&c).size();
        size_t u = index[pa[d]], v = index[pb[d]];
        if (u != v) {
            links.insert({u, v});
        }
        label_w = std::max(label_w, measure_label(vg, e.label, edge_fs));
    }

    std::vector<double> kx(n), ky(n);
    double KW = 0, KH = 0;
    std::string dir = c.layout.empty() ? (links.empty() ? "grid" : "right") : c.layout;
    if (dir == "grid" || links.empty()) {
        size_t cols = dir == "right" ? n : dir == "down" ? 1
                                         : size_t(std::ceil(std::sqrt(double(n))));
        cols = std::max<size_t>(cols, 1);
        size_t rows = (n + cols - 1) / cols;
        std::vector<double> cw(cols), rh(rows);
        for (size_t i = 0; i < n; ++i) {
            cw[i % cols] = std::max(cw[i % cols], slots[i].w);
            rh[i / cols] = std::max(rh[i / cols], slots[i].h + slots[i].top);
        }
        double g = gap * 1.5;
        for (size_t i = 0; i < n; ++i) {
            double x = 0, y = 0;
            for (size_t j = 0; j < i % cols; ++j) {
                x += cw[j] + g;
            }
            for (size_t j = 0; j < i / cols; ++j) {
                y += rh[j] + g;
            }
            kx[i] = x + (cw[i % cols] - slots[i].w) / 2;
            ky[i] = y + (rh[i / cols] - slots[i].h - slots[i].top) / 2 + slots[i].top;
        }
        for (double v : cw) {
            KW += v + g;
        }
        for (double v : rh) {
            KH += v + g;
        }
        KW = std::max(0.0, KW - g);
        KH = std::max(0.0, KH - g);
    } else {
        // Layered: rank = longest path from a source (back edges of cycles
        // ignored), order within a rank by the average position of neighbours.
        std::vector<std::vector<size_t>> out(n), in(n);
        std::vector<int> state(n, 0);
        std::set<std::pair<size_t, size_t>> dag;
        std::function<void(size_t)> dfs = [&](size_t u) {
            state[u] = 1;
            for (auto [a, b] : links) {
                if (a != u) {
                    continue;
                }
                if (state[b] == 1) {
                    continue; // back edge
                }
                dag.insert({a, b});
                if (state[b] == 0) {
                    dfs(b);
                }
            }
            state[u] = 2;
        };
        for (size_t i = 0; i < n; ++i) {
            if (state[i] == 0) {
                dfs(i);
            }
        }
        std::vector<int> rank(n, 0), indeg(n, 0);
        std::vector<bool> linked(n, false);
        for (auto [a, b] : dag) {
            out[a].push_back(b);
            in[b].push_back(a);
            ++indeg[b];
        }
        for (auto [a, b] : links) {
            linked[a] = linked[b] = true;
        }
        std::vector<size_t> queue;
        for (size_t i = 0; i < n; ++i) {
            if (indeg[i] == 0) {
                queue.push_back(i);
            }
        }
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            size_t u = queue[qi];
            for (size_t v : out[u]) {
                rank[v] = std::max(rank[v], rank[u] + 1);
                if (--indeg[v] == 0) {
                    queue.push_back(v);
                }
            }
        }
        int max_rank = 0;
        for (size_t i = 0; i < n; ++i) {
            if (linked[i]) {
                max_rank = std::max(max_rank, rank[i]);
            }
        }
        for (size_t i = 0; i < n; ++i) {
            if (!linked[i]) {
                rank[i] = max_rank + 1; // loose boxes: one extra column at the end
            }
        }
        int ranks = *std::max_element(rank.begin(), rank.end()) + 1;
        size_t nr = size_t(ranks);
        std::vector<std::vector<size_t>> R(nr);
        for (size_t i = 0; i < n; ++i) {
            R[size_t(rank[i])].push_back(i);
        }
        std::vector<double> pos(n);
        auto number = [&] {
            for (auto& r : R) {
                for (size_t j = 0; j < r.size(); ++j) {
                    pos[r[j]] = double(j);
                }
            }
        };
        number();
        auto sweep = [&](bool down) {
            for (int s = 0; s < ranks; ++s) {
                auto& r = R[size_t(down ? s : ranks - 1 - s)];
                std::vector<double> key(n);
                for (size_t v : r) {
                    const auto& nb = down ? in[v] : out[v];
                    double sum = 0;
                    for (size_t u : nb) {
                        sum += pos[u];
                    }
                    key[v] = nb.empty() ? pos[v] : sum / double(nb.size());
                }
                std::stable_sort(r.begin(), r.end(),
                                 [&](size_t a, size_t b) { return key[a] < key[b]; });
                number();
            }
        };
        for (int it = 0; it < 2; ++it) {
            sweep(true);
            sweep(false);
        }

        bool right = dir == "right";
        double rank_gap = right ? std::max(130.0, label_w + 70) : 120;
        // Main axis: along the flow; cross axis: across it.
        auto main_len = [&](size_t i) { return right ? slots[i].w : slots[i].h + slots[i].top; };
        auto cross_len = [&](size_t i) { return right ? slots[i].h + slots[i].top : slots[i].w; };
        std::vector<double> rank_len(nr), rank_cross(nr);
        for (int r = 0; r < ranks; ++r) {
            for (size_t i : R[size_t(r)]) {
                rank_len[size_t(r)] = std::max(rank_len[size_t(r)], main_len(i));
                rank_cross[size_t(r)] += cross_len(i) + gap;
            }
            rank_cross[size_t(r)] -= gap;
        }
        // A long flow wraps into several lines of ranks (rows for `right`,
        // columns for `down`) so the level comes out roughly 16:10, not a
        // thin strip.
        double cross_max = *std::max_element(rank_cross.begin(), rank_cross.end());
        double total = -rank_gap;
        for (double l : rank_len) {
            total += l + rank_gap;
        }
        double want = right ? std::sqrt(total / (1.6 * cross_max))
                            : std::sqrt(1.6 * total / cross_max);
        int n_lines = std::clamp(int(std::lround(want)), 1, std::max(1, ranks / 2));
        std::vector<std::vector<size_t>> lines(1);
        double acc = 0;
        for (size_t r = 0; r < nr; ++r) {
            if (!lines.back().empty() && acc + rank_len[r] > total / n_lines * 1.1 &&
                int(lines.size()) < n_lines) {
                lines.emplace_back();
                acc = 0;
            }
            lines.back().push_back(r);
            acc += rank_len[r] + rank_gap;
        }
        double c0 = 0, main_max = 0;
        for (const auto& line : lines) {
            double lc = 0, m = 0;
            for (size_t r : line) {
                lc = std::max(lc, rank_cross[r]);
            }
            for (size_t r : line) {
                double cpos = c0 + (lc - rank_cross[r]) / 2;
                for (size_t i : R[r]) {
                    double mpos = m + (rank_len[r] - main_len(i)) / 2;
                    if (right) {
                        kx[i] = mpos;
                        ky[i] = cpos + slots[i].top;
                    } else {
                        kx[i] = cpos;
                        ky[i] = mpos + slots[i].top;
                    }
                    cpos += cross_len(i) + gap;
                }
                m += rank_len[r] + rank_gap;
            }
            main_max = std::max(main_max, m - rank_gap);
            c0 += lc + rank_gap; // room for the arrows that wrap around
        }
        c0 -= rank_gap;
        KW = right ? main_max : c0;
        KH = right ? c0 : main_max;
    }

    // Notes on top when the boxes are wide, on the left when they are tall.
    double NW = 0, NH = 0;
    for (auto& nn : c.notes) {
        NW = std::max(NW, nn->w);
        NH += nn->h + 16;
    }
    NH = std::max(0.0, NH - 16);
    bool side = n > 0 && !c.notes.empty() && KH > KW;
    double ox = pad, oy = pad; // where the kids start
    double y = pad;
    for (auto& nn : c.notes) {
        nn->x = pad;
        nn->y = y;
        y += nn->h + 16;
    }
    if (!c.notes.empty() && n > 0) {
        if (side) {
            ox += NW + gap;
        } else {
            oy += NH + gap;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        Item& k = *c.kids[i];
        k.x = ox + kx[i];
        k.y = oy + ky[i];
        k.w = slots[i].w;
        k.h = slots[i].h;
    }
    c.W = std::max({side ? NW + gap + KW : std::max(NW, KW), 200.0}) + 2 * pad;
    c.H = (side ? std::max(NH, KH) : NH + (n && NH > 0 ? gap : 0) + KH) + 2 * pad;
    c.H = std::max(c.H, 120.0);
}

// Where the line from the center of `r` toward `p` leaves the shape.
Vec2 outline(Kind k, Rect r, Vec2 p, double gap_units) {
    Vec2 c = r.center(), d = p - c;
    double len = length(d), hw = r.w / 2, hh = r.h / 2;
    if (len < 1e-12) {
        return c;
    }
    double ax = std::abs(d.x), ay = std::abs(d.y), t;
    if (k == Kind::Ellipse) {
        t = 1 / std::sqrt((d.x / hw) * (d.x / hw) + (d.y / hh) * (d.y / hh));
    } else if (k == Kind::Diamond) {
        t = 1 / (ax / hw + ay / hh);
    } else {
        t = std::min(ax > 0 ? hw / ax : 1e300, ay > 0 ? hh / ay : 1e300);
    }
    return c + d * std::min(t, 1.0) + d * (gap_units / len);
}

// Rect of `it` in the layout units of its ancestor `c`.
Rect rect_in(const Item* it, const Item* c) {
    Rect r{it->x, it->y, it->w, it->h};
    for (const Item* p = it->parent; p && p != c; p = p->parent) {
        double k = p->w / p->W; // p's content units -> p's parent's units
        r = {p->x + r.x * k, p->y + r.y * k, r.w * k, r.h * k};
    }
    return r;
}

// Makes the node for `it` (and everything inside it) in normalized units:
// every level is 1 unit wide.
std::unique_ptr<Node> build_node(Item& it, const std::string& file, double parent_W) {
    auto n = std::make_unique<Node>();
    n->id = hash_id(file + "/" + it.id);
    n->seed = uint32_t(std::hash<std::string>{}(n->id)) | 1u;
    n->kind = it.kind;
    n->text = it.label;
    n->src = file;
    n->tags = it.tags;
    double u = 1 / parent_W;
    n->x = it.x * u;
    n->y = it.y * u;
    n->w = it.w * u;
    n->h = it.h * u;
    n->style.fill = fill_of(it.hue);
    n->style.stroke = stroke_of(it.hue);
    n->style.fill_style = it.fill;
    n->style.roughness = it.rough;
    n->style.dash = it.dash;
    n->style.stroke_width = stroke * it.weight * u;
    n->style.font_size = (it.container() ? title_fs : label_fs) * u;
    it.node = n.get();

    double cu = 1 / it.W;
    for (size_t i = 0; i < it.notes.size(); ++i) {
        auto& nn = it.notes[i];
        nn->id = hash_id(file + "/" + it.id + "/note" + std::to_string(i));
        nn->seed = uint32_t(std::hash<std::string>{}(nn->id)) | 1u;
        nn->src = file;
        nn->x *= cu;
        nn->y *= cu;
        nn->w *= cu;
        nn->h *= cu;
        nn->style.font_size *= cu;
        add_child(*n, std::move(nn));
    }
    it.notes.clear();
    for (Item* k : it.kids) {
        add_child(*n, build_node(*k, file, it.W));
    }
    return n;
}

} // namespace

std::string hash_id(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h = (h ^ c) * 1099511628211ull;
    }
    return std::format("{:012x}", h & 0xffffffffffffull);
}

Result build(NVGcontext* vg, const std::string& source, const std::string& name) {
    Result res;
    Parsed P;
    try {
        parse(source, P);
        std::set<std::string> concept_ids;
        for (auto& c : P.concepts) {
            concept_ids.insert(c.id);
        }
        auto check_tags = [&](const std::vector<std::string>& tags, int line) {
            for (auto& t : tags) {
                if (!concept_ids.contains(t)) {
                    throw Error{line, "unknown concept '#" + t + "' (add: concept " + t +
                                          ": Name)"};
                }
            }
        };
        for (Item& it : P.items) {
            check_tags(it.tags, it.line);
        }
        for (Edge& e : P.edges) {
            check_tags(e.tags, e.line);
            for (auto [id, ptr] : {std::pair{&e.from, &e.a}, {&e.to, &e.b}}) {
                auto f = P.by_id.find(*id);
                if (f == P.by_id.end()) {
                    throw Error{e.line, "unknown box '" + *id + "'"};
                }
                *ptr = f->second;
            }
            auto pa = path_of(e.a), pb = path_of(e.b);
            size_t i = 0;
            while (i < pa.size() && i < pb.size() && pa[i] == pb[i]) {
                ++i;
            }
            if (i == pa.size() || i == pb.size()) {
                throw Error{e.line, "can't draw an arrow between a box and something "
                                    "inside it ('" + e.from + "', '" + e.to + "')"};
            }
            e.lca = pa[i - 1];
            // Steps of a flow light up the boxes they connect.
            for (auto& t : e.tags) {
                for (Item* x : {e.a, e.b}) {
                    if (std::find(x->tags.begin(), x->tags.end(), t) == x->tags.end()) {
                        x->tags.push_back(t);
                    }
                }
            }
        }
    } catch (const Error& e) {
        res.error = std::format("line {}: {}", e.line, e.msg);
        return res;
    }

    // Colors: unset boxes get hues their siblings and parent don't use.
    for (Item& it : P.items) {
        int used[ui::n_hues] = {};
        for (Item* k : it.kids) {
            if (k->hue >= 0) {
                ++used[k->hue];
            }
        }
        if (it.hue >= 0) {
            used[it.hue] += 1000; // never the parent's own color
        }
        for (Item* k : it.kids) {
            if (k->hue != -1) {
                continue;
            }
            int best = ui::auto_order[0];
            for (int h : ui::auto_order) {
                if (used[h] < used[best]) {
                    best = h;
                }
            }
            k->hue = best;
            ++used[best];
        }
    }

    Item& frame = P.items.front();
    if (frame.label.empty()) {
        frame.label = name.substr(0, name.rfind('.'));
    }
    frame.hue = -4;
    frame.rough = 0;
    layout(vg, frame, P.edges);
    frame.x = frame.y = 0;
    frame.w = 1;
    frame.h = frame.H / frame.W;
    auto root = build_node(frame, name, 1);
    root->style.stroke = 0x868e96ff;
    root->style.stroke_width = 0.0015;
    root->style.font_size = 0.03;

    // Arrows live in the deepest box that holds both ends.
    std::map<std::pair<Item*, Item*>, int> pair_count, pair_seen;
    for (Edge& e : P.edges) {
        ++pair_count[{std::min(e.a, e.b), std::max(e.a, e.b)}];
    }
    std::map<std::string, int> seen_id;
    for (Edge& e : P.edges) {
        Item* c = e.lca;
        double u = 1 / c->W; // layout units -> c's local units
        Rect ra = rect_in(e.a, c), rb = rect_in(e.b, c);
        auto key = std::pair{std::min(e.a, e.b), std::max(e.a, e.b)};
        int cnt = pair_count[key], k = pair_seen[key]++;
        // Several arrows between the same two boxes bow apart.
        Vec2 ca = ra.center(), cb = rb.center(), mid = (ca + cb) * 0.5;
        Vec2 d = cb - ca;
        double len = std::max(length(d), 1e-12);
        // (Measured the same way for a -> b and b -> a, so they part.)
        double side = (k - (cnt - 1) / 2.0) * 50 * (e.a == key.first ? 1 : -1);
        Vec2 bend = mid + Vec2{-d.y / len, d.x / len} * side;
        bool bent = std::abs(side) > 0;
        Vec2 p0 = outline(e.a->kind, ra, bent ? bend : cb, 6);
        Vec2 p1 = outline(e.b->kind, rb, bent ? bend : ca, 6);
        std::vector<Vec2> pts{p0 * u};
        if (bent) {
            pts.push_back(bend * u);
        }
        pts.push_back(p1 * u);

        auto n = std::make_unique<Node>();
        std::string base = name + "/" + e.from + ">" + e.to;
        n->id = hash_id(base + "#" + std::to_string(seen_id[base]++));
        n->seed = uint32_t(std::hash<std::string>{}(n->id)) | 1u;
        n->kind = e.kind;
        n->text = e.label;
        n->src = name;
        n->tags = e.tags;
        n->style.stroke = e.hue >= 0 ? ui::shade(e.hue, 4) : e.hue == -2 ? 0x868e96ff : 0x343a40ff;
        n->style.stroke_width = stroke * e.weight * u;
        n->style.font_size = edge_fs * u;
        n->style.dash = e.dash;
        n->style.start_arrow = e.both;
        n->style.roughness = 1;
        n->start_bind = e.a->node->id;
        n->end_bind = e.b->node->id;
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (Vec2 p : pts) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
        double pd = n->style.stroke_width / 2;
        n->x = x0 - pd;
        n->y = y0 - pd;
        n->w = x1 - x0 + 2 * pd;
        n->h = y1 - y0 + 2 * pd;
        for (Vec2 p : pts) {
            n->points.push_back({(p.x - n->x) / n->w, (p.y - n->y) / n->h});
        }
        add_child(*c->node, std::move(n));
    }

    for (auto& cpt : P.concepts) {
        while (!cpt.note.empty() && cpt.note.back() == '\n') {
            cpt.note.pop_back();
        }
    }
    res.frame = std::move(root);
    res.concepts = std::move(P.concepts);
    return res;
}

} // namespace diagram
