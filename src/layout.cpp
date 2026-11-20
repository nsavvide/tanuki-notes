#include "layout.hpp"

#include "render.hpp"
#include "text.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace layout {

namespace {

// Layout units: roughly screen pixels when a level fills the window.
constexpr double label_fs = 20, title_fs = 22, note_fs = 16, code_fs = 14, edge_fs = 16;
constexpr double pad = 48, gap = 56, stroke = 2;

bool is_note(const Node& n) { return n.kind == Kind::Text || n.kind == Kind::Code; }
bool is_shape(const Node& n) {
    return n.kind == Kind::Rect || n.kind == Kind::Ellipse || n.kind == Kind::Diamond;
}
bool unplaced(const Node& n) { return n.w <= 0; }
bool has_content(const Node& n) {
    return std::any_of(n.children.begin(), n.children.end(),
                       [](auto& c) { return !c->is_linear(); });
}

struct Ctx {
    NVGcontext* vg;
    std::unordered_map<std::string, Node*> by_id;
    std::vector<Node*> arrows; // bound at both ends
};

// The child of `c` that is or holds `n`.
Node* child_toward(const Node* c, Node* n) {
    while (n && n->parent != c) {
        n = n->parent;
    }
    return n;
}

double measure(NVGcontext* vg, const std::string& s, double fs, int* lines = nullptr) {
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

struct Item {
    Node* n;
    double x = 0, y = 0, w = 0, h = 0;
    double top = 0;    // room above for a container's title
    double aspect = 0; // a container's content h / w; 0 for a leaf
};

double content(Ctx& cx, Node& c); // lays out all of c's contents, returns h / w

// Sizes in layout units.
void size_item(Ctx& cx, Item& it) {
    Node& n = *it.n;
    if (is_note(n)) {
        n.style.font_size = n.kind == Kind::Code ? code_fs : note_fs;
        if (n.kind == Kind::Code) {
            fit_code_bounds(cx.vg, n);
        } else {
            n.w = 0;
            fit_text_bounds(cx.vg, n);
        }
        it.w = n.w;
        it.h = n.h;
        return;
    }
    int lines;
    double tw = measure(cx.vg, n.text, label_fs, &lines);
    if (has_content(n)) {
        // Bigger the more it holds; its shape follows its content.
        it.aspect = content(cx, n);
        size_t k = std::count_if(n.children.begin(), n.children.end(),
                                 [](auto& c) { return !c->is_linear(); });
        double area = 26000 * double(k + 2);
        it.w = std::max(std::sqrt(area / it.aspect), measure(cx.vg, n.text, title_fs) + 20);
        it.h = it.w * it.aspect;
        int tl;
        measure(cx.vg, n.text, title_fs, &tl);
        it.top = tl * title_fs * text::line_height + 6;
        return;
    }
    it.w = std::max(150.0, tw + 56);
    it.h = std::max(76.0, lines * label_fs * 1.25 + 44);
    if (n.kind == Kind::Ellipse) {
        it.w *= 1.3;
        it.h *= 1.35;
    } else if (n.kind == Kind::Diamond) {
        it.w *= 1.6;
        it.h *= 1.6;
    }
}

// Places `items` (children of `c`) in layout units: boxes layered along the
// arrows between them (or in a grid), notes above or beside them. Returns the
// size including padding.
std::pair<double, double> arrange(Ctx& cx, const Node& c, std::vector<Item>& items) {
    std::vector<Item*> boxes, notes;
    for (auto& it : items) {
        size_item(cx, it);
        (is_note(*it.n) ? notes : boxes).push_back(&it);
    }
    size_t n = boxes.size();
    std::map<const Node*, size_t> index;
    for (size_t i = 0; i < n; ++i) {
        index[boxes[i]->n] = i;
    }
    std::set<std::pair<size_t, size_t>> links;
    double label_w = 0;
    for (Node* a : cx.arrows) {
        Node* u = child_toward(&c, cx.by_id[a->start_bind]);
        Node* v = child_toward(&c, cx.by_id[a->end_bind]);
        if (u && v && u != v && index.contains(u) && index.contains(v)) {
            links.insert({index[u], index[v]});
            label_w = std::max(label_w, measure(cx.vg, a->text, edge_fs));
        }
    }

    std::vector<double> kx(n), ky(n);
    double KW = 0, KH = 0;
    auto slot_h = [&](size_t i) { return boxes[i]->h + boxes[i]->top; };
    if (links.empty()) {
        size_t cols = std::max<size_t>(1, size_t(std::ceil(std::sqrt(double(n)))));
        size_t rows = (n + cols - 1) / cols;
        std::vector<double> cw(cols), rh(rows);
        for (size_t i = 0; i < n; ++i) {
            cw[i % cols] = std::max(cw[i % cols], boxes[i]->w);
            rh[i / cols] = std::max(rh[i / cols], slot_h(i));
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
            kx[i] = x + (cw[i % cols] - boxes[i]->w) / 2;
            ky[i] = y + (rh[i / cols] - slot_h(i)) / 2 + boxes[i]->top;
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
        // Layered, left to right: rank = longest path from a source (back
        // edges of cycles ignored); order within a rank by the average
        // position of neighbours. Long flows wrap into rows (about 16:10).
        std::vector<std::vector<size_t>> out(n), in(n);
        std::vector<int> state(n, 0);
        std::set<std::pair<size_t, size_t>> dag;
        std::function<void(size_t)> dfs = [&](size_t u) {
            state[u] = 1;
            for (auto [a, b] : links) {
                if (a == u && state[b] != 1) {
                    dag.insert({a, b});
                    if (state[b] == 0) {
                        dfs(b);
                    }
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
            for (size_t v : out[queue[qi]]) {
                rank[v] = std::max(rank[v], rank[queue[qi]] + 1);
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
                rank[i] = max_rank + 1; // loose boxes: one extra column
            }
        }
        size_t nr = size_t(*std::max_element(rank.begin(), rank.end()) + 1);
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
            for (size_t s = 0; s < nr; ++s) {
                auto& r = R[down ? s : nr - 1 - s];
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

        double rank_gap = std::max(130.0, label_w + 70);
        std::vector<double> rank_len(nr), rank_cross(nr);
        for (size_t r = 0; r < nr; ++r) {
            for (size_t i : R[r]) {
                rank_len[r] = std::max(rank_len[r], boxes[i]->w);
                rank_cross[r] += slot_h(i) + gap;
            }
            rank_cross[r] -= gap;
        }
        double cross_max = *std::max_element(rank_cross.begin(), rank_cross.end());
        double total = -rank_gap;
        for (double l : rank_len) {
            total += l + rank_gap;
        }
        int n_lines = std::clamp(int(std::lround(std::sqrt(total / (1.6 * cross_max)))), 1,
                                 std::max(1, int(nr) / 2));
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
        double c0 = 0;
        for (const auto& line : lines) {
            double lc = 0, m = 0;
            for (size_t r : line) {
                lc = std::max(lc, rank_cross[r]);
            }
            for (size_t r : line) {
                double cpos = c0 + (lc - rank_cross[r]) / 2;
                for (size_t i : R[r]) {
                    kx[i] = m + (rank_len[r] - boxes[i]->w) / 2;
                    ky[i] = cpos + boxes[i]->top;
                    cpos += slot_h(i) + gap;
                }
                m += rank_len[r] + rank_gap;
            }
            KW = std::max(KW, m - rank_gap);
            c0 += lc + rank_gap; // room for arrows that wrap around
        }
        KH = c0 - rank_gap;
    }

    // Notes on top when the boxes are wide, beside them when they are tall.
    double NW = 0, NH = 0;
    for (Item* it : notes) {
        NW = std::max(NW, it->w);
        NH += it->h + 16;
    }
    NH = std::max(0.0, NH - 16);
    bool side = n > 0 && !notes.empty() && KH > KW;
    double y = pad;
    for (Item* it : notes) {
        it->x = pad;
        it->y = y;
        y += it->h + 16;
    }
    double ox = pad, oy = pad;
    if (!notes.empty() && n > 0) {
        (side ? ox : oy) += (side ? NW : NH) + gap;
    }
    for (size_t i = 0; i < n; ++i) {
        boxes[i]->x = ox + kx[i];
        boxes[i]->y = oy + ky[i];
    }
    double W = std::max(side ? NW + gap + KW : std::max(NW, KW), 200.0) + 2 * pad;
    double H = std::max((side ? std::max(NH, KH) : NH + (n && NH > 0 ? gap : 0) + KH) + 2 * pad,
                        120.0);
    return {W, H};
}

// A light fill and dark outline in a hue the siblings and parent don't use.
void auto_color(Node& n) {
    int used[ui::n_hues] = {};
    for (const auto& c : n.parent->children) {
        if (c.get() != &n) {
            if (int h = ui::hue_of(c->style.fill); h >= 0) {
                ++used[h];
            }
        }
    }
    if (int ph = ui::hue_of(n.parent->style.fill); ph >= 0) {
        used[ph] += 1000;
    }
    int best = ui::auto_order[0];
    for (int h : ui::auto_order) {
        if (used[h] < used[best]) {
            best = h;
        }
    }
    n.style.fill = ui::shade(best, ui::auto_fill_shade);
    n.style.stroke = ui::shade(best, ui::auto_stroke_shade);
    n.style.fill_style = FillStyle::Solid;
}

// Puts laid-out items into the parent's units: layout unit -> `u`, offset.
void apply(std::vector<Item>& items, double u, double ox, double oy) {
    for (Item& it : items) {
        Node& n = *it.n;
        n.x = ox + it.x * u;
        n.y = oy + it.y * u;
        n.w = it.w * u;
        n.h = it.h * u;
        if (is_note(n)) {
            n.style.font_size *= u;
            continue;
        }
        n.style.font_size = (it.aspect > 0 ? title_fs : label_fs) * u;
        n.style.stroke_width = stroke * u;
        if (n.style.fill == 0 && is_shape(n)) {
            auto_color(n);
        }
    }
}

double content(Ctx& cx, Node& c) {
    std::vector<Item> items;
    for (auto& k : c.children) {
        if (!k->is_linear()) {
            items.push_back({k.get()});
        }
    }
    auto [W, H] = arrange(cx, c, items);
    apply(items, 1 / W, 0, 0);
    return H / W;
}

// Layout units -> c's local units, judged from what's already in c.
double unit_of(const Node& c) {
    std::vector<double> u;
    for (const auto& k : c.children) {
        if (k->is_linear() || unplaced(*k) || k->style.font_size <= 0) {
            continue;
        }
        double fs = k->kind == Kind::Text   ? note_fs
                    : k->kind == Kind::Code ? code_fs
                    : has_content(*k)       ? title_fs
                                            : label_fs;
        u.push_back(k->style.font_size / fs);
    }
    if (u.empty()) {
        return 0;
    }
    std::nth_element(u.begin(), u.begin() + long(u.size() / 2), u.end());
    return u[u.size() / 2];
}

// Makes `c` taller (in its parent's units), moving what sits below it down
// by as much, and growing its parent in turn if it no longer fits.
void grow(Node& c, double new_h) {
    if (c.kind == Kind::Root || new_h <= c.h) {
        return;
    }
    double old_bottom = c.y + c.h, d = new_h - c.h;
    c.h = new_h;
    Node& p = *c.parent;
    double bottom = 0;
    for (auto& s : p.children) {
        if (s.get() != &c && !s->is_linear() && s->y >= old_bottom - 1e-12 * p.w &&
            s->x < c.x + c.w && s->x + s->w > c.x) {
            s->y += d;
        }
        if (!s->is_linear()) {
            bottom = std::max(bottom, s->y + s->h);
        }
    }
    if (p.kind != Kind::Root && bottom > p.h / p.w) {
        grow(p, bottom * p.w);
    }
}

void place(Ctx& cx, Node& c) {
    std::vector<Item> items;
    double x0 = 1e300, y1 = -1e300;
    for (auto& k : c.children) {
        if (k->is_linear()) {
            continue;
        }
        if (unplaced(*k)) {
            items.push_back({k.get()});
            continue;
        }
        x0 = std::min(x0, k->x);
        y1 = std::max(y1, k->y + k->h);
        if (k->is_container()) {
            place(cx, *k);
        }
    }
    if (items.empty()) {
        return;
    }
    double u = unit_of(c);
    auto [W, H] = arrange(cx, c, items);
    double ox, oy;
    if (u > 0) { // below what's there, same scale (smaller if it wouldn't fit across)
        if (c.kind != Kind::Root) {
            u = std::min(u, 1 / W);
        }
        ox = c.kind == Kind::Root ? x0 - pad * u : std::clamp(x0 - pad * u, 0.0, 1 - W * u);
        oy = y1 + (gap - pad) * u;
    } else if (c.kind == Kind::Root) { // an empty map: about a screen wide
        u = 0.001 / W;
        ox = 0.5 - 0.0005;
        oy = 0.5 - H * u / 2;
    } else { // an empty box: fit inside it, as it is
        double a = c.h / c.w;
        u = std::min(1 / W, a / H);
        ox = (1 - W * u) / 2;
        oy = (a - H * u) / 2;
    }
    apply(items, u, ox, oy);
    if (c.kind != Kind::Root && (oy + H * u) * c.w > c.h) {
        grow(c, (oy + H * u) * c.w); // make room for the new things
    }
}

// `n`'s box in the local units of its ancestor `a`.
Rect rect_in(const Node* n, const Node* a) {
    Rect r = n->rect();
    for (const Node* p = n->parent; p && p != a; p = p->parent) {
        r = {p->x + r.x * p->w, p->y + r.y * p->w, r.w * p->w, r.h * p->w};
    }
    return r;
}

Vec2 outline(const Node& n, Rect r, Vec2 toward, double g) {
    Vec2 c = r.center(), d = toward - c;
    double len = length(d), hw = r.w / 2, hh = r.h / 2;
    if (len < 1e-300) {
        return c;
    }
    double ax = std::abs(d.x), ay = std::abs(d.y), t;
    if (n.kind == Kind::Ellipse) {
        t = 1 / std::sqrt((d.x / hw) * (d.x / hw) + (d.y / hh) * (d.y / hh));
    } else if (n.kind == Kind::Diamond) {
        t = 1 / (ax / hw + ay / hh);
    } else {
        t = std::min(ax > 0 ? hw / ax : 1e300, ay > 0 ? hh / ay : 1e300);
    }
    return c + d * std::min(t, 1.0) + d * (g / len);
}

void place_arrow(Node& a, Node& s, Node& e) {
    if (!is_ancestor(a.parent, &s) || !is_ancestor(a.parent, &e)) {
        return;
    }
    double u = unit_of(*a.parent);
    if (u <= 0) {
        u = std::max(s.w, e.w) / 150;
    }
    a.style.stroke_width = stroke * u;
    a.style.font_size = edge_fs * u;
    Rect rs = rect_in(&s, a.parent), re = rect_in(&e, a.parent);
    std::vector<Vec2> pts{outline(s, rs, re.center(), 6 * u), outline(e, re, rs.center(), 6 * u)};
    double x0 = std::min(pts[0].x, pts[1].x), y0 = std::min(pts[0].y, pts[1].y);
    double pd = a.style.stroke_width / 2;
    a.x = x0 - pd;
    a.y = y0 - pd;
    a.w = std::abs(pts[1].x - pts[0].x) + 2 * pd;
    a.h = std::abs(pts[1].y - pts[0].y) + 2 * pd;
    a.points.clear();
    for (Vec2 p : pts) {
        a.points.push_back({(p.x - a.x) / a.w, (p.y - a.y) / a.h});
    }
}

} // namespace

void place_new(NVGcontext* vg, Node& root) {
    Ctx cx{vg, {}, {}};
    std::function<void(Node&)> walk = [&](Node& n) {
        cx.by_id[n.id] = &n;
        for (auto& c : n.children) {
            walk(*c);
        }
    };
    walk(root);
    for (auto& [id, n] : cx.by_id) {
        if (n->is_linear() && cx.by_id.contains(n->start_bind) &&
            cx.by_id.contains(n->end_bind)) {
            cx.arrows.push_back(n);
        }
    }
    std::sort(cx.arrows.begin(), cx.arrows.end(),
              [](Node* a, Node* b) { return a->id < b->id; }); // deterministic
    place(cx, root);
    for (Node* a : cx.arrows) {
        if (a->points.empty()) {
            place_arrow(*a, *cx.by_id[a->start_bind], *cx.by_id[a->end_bind]);
        }
    }
}

} // namespace layout
