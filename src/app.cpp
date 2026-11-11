#include "app.hpp"
#include "code.hpp"
#include "note.hpp"
#include "render.hpp"
#include "rough.hpp"
#include "store.hpp"
#include "text.hpp"

#include <nanovg.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <format>
#include <map>
#include <unordered_map>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr double max_scale = 1e12;   // camera px per anchor unit
constexpr double draw_root_max = 1e9; // ancestors larger than this are skipped
constexpr double initial_root_px = 1e6;

bool covers(const Rect& r, int w, int h) {
    return r.x <= 0 && r.y <= 0 && r.right() >= w && r.bottom() >= h;
}

// Placement of `n`'s parent, given `n`'s placement.
Xform parent_xform(const Node& n, Xform t) {
    double s = t.s / n.w;
    return {t.ox - n.x * s, t.oy - n.y * s, s};
}

double dist_to_segment(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a;
    double len2 = ab.x * ab.x + ab.y * ab.y;
    double k = len2 > 0 ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2,
                                     0.0, 1.0)
                        : 0.0;
    return length(p - (a + ab * k));
}

// Ramer-Douglas-Peucker: drops points that add no visible detail.
void simplify(const std::vector<Vec2>& in, double tol, std::vector<Vec2>& out) {
    if (in.size() < 3) {
        out = in;
        return;
    }
    std::vector<bool> keep(in.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<size_t, size_t>> stack{{0, in.size() - 1}};
    while (!stack.empty()) {
        auto [a, b] = stack.back();
        stack.pop_back();
        double best = 0;
        size_t idx = a;
        for (size_t i = a + 1; i < b; ++i) {
            double d = dist_to_segment(in[i], in[a], in[b]);
            if (d > best) {
                best = d;
                idx = i;
            }
        }
        if (best > tol) {
            keep[idx] = true;
            stack.push_back({a, idx});
            stack.push_back({idx, b});
        }
    }
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (keep[i]) {
            out.push_back(in[i]);
        }
    }
}

// Fresh ids for copies; `remap` records old -> new so attached arrows inside
// the copied group can follow along.
void regen_ids(Node& n, std::map<std::string, std::string>& remap) {
    std::string old = n.id;
    n.id = new_id();
    remap[old] = n.id;
    for (auto& c : n.children) {
        regen_ids(*c, remap);
    }
}

// Attachments to shapes outside the copied group are dropped.
void remap_binds(Node& n, const std::map<std::string, std::string>& remap) {
    for (std::string* b : {&n.start_bind, &n.end_bind}) {
        if (!b->empty()) {
            auto it = remap.find(*b);
            *b = it == remap.end() ? "" : it->second;
        }
    }
    for (auto& c : n.children) {
        remap_binds(*c, remap);
    }
}

// Where the line from the shape's center toward `toward` leaves the shape,
// pushed out by `gap`. nullopt if `toward` is inside the shape.
std::optional<Vec2> boundary_point(const Node& n, Rect r, Vec2 toward, double gap) {
    Vec2 c = r.center();
    Vec2 d = toward - c;
    double len = length(d);
    double hw = r.w / 2, hh = r.h / 2;
    if (len < 1e-9 || hw <= 0 || hh <= 0) {
        return std::nullopt;
    }
    double ax = std::abs(d.x), ay = std::abs(d.y), t;
    if (n.kind == Kind::Ellipse) {
        t = 1 / std::sqrt((d.x / hw) * (d.x / hw) + (d.y / hh) * (d.y / hh));
    } else if (n.kind == Kind::Diamond) {
        t = 1 / (ax / hw + ay / hh);
    } else {
        t = std::min(ax > 0 ? hw / ax : 1e300, ay > 0 ? hh / ay : 1e300);
    }
    if (t >= 1) {
        return std::nullopt;
    }
    return c + d * t + d * (gap / len);
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = char(std::tolower((unsigned char)c));
    }
    return s;
}

size_t prev_cp(const std::string& s, size_t i) {
    if (i == 0) {
        return 0;
    }
    --i;
    while (i > 0 && (s[i] & 0xc0) == 0x80) {
        --i;
    }
    return i;
}

size_t next_cp(const std::string& s, size_t i) {
    if (i >= s.size()) {
        return s.size();
    }
    ++i;
    while (i < s.size() && (s[i] & 0xc0) == 0x80) {
        ++i;
    }
    return i;
}

// Byte index where the line containing `i` starts.
size_t line_start(const std::string& s, size_t i) {
    if (i == 0) {
        return 0;
    }
    size_t nl = s.rfind('\n', i - 1);
    return nl == std::string::npos ? 0 : nl + 1;
}

Kind kind_for(Tool t) {
    switch (t) {
    case Tool::Diamond: return Kind::Diamond;
    case Tool::Ellipse: return Kind::Ellipse;
    case Tool::Arrow: return Kind::Arrow;
    case Tool::Line: return Kind::Line;
    case Tool::Draw: return Kind::Draw;
    default: return Kind::Rect;
    }
}

bool is_shape_tool(Tool t) {
    return t == Tool::Rect || t == Tool::Diamond || t == Tool::Ellipse ||
           t == Tool::Arrow || t == Tool::Line || t == Tool::Draw;
}

} // namespace

// ---------------------------------------------------------------- lifecycle

App::App(NVGcontext* vg, SDL_Window* window, fs::path map_dir)
    : vg_(vg), window_(window), dir_(std::move(map_dir)), assets_(vg, dir_) {
    fs::create_directories(dir_);
    style_.fill_style = FillStyle::Solid; // calmer under auto colors
    root_ = store::load(dir_);
    // Note heights follow the current layout (older maps used another font).
    std::function<void(Node&)> refit = [&](Node& n) {
        if (n.kind == Kind::Text) {
            fit_text_bounds(vg_, n);
        }
        for (auto& c : n.children) {
            refit(*c);
        }
    };
    refit(*root_);
    anchor_ = root_.get();
    SDL_GetWindowSize(window_, &win_w_, &win_h_);

    store::View view = store::load_view(dir_);
    Node* a = view.valid ? find_by_id(*root_, view.anchor_id) : nullptr;
    if (a && view.camera.scale > 0) {
        anchor_ = a;
        cam_ = view.camera;
    } else {
        cam_.scale = initial_root_px;
        cam_.x = 0.5 - win_w_ / 2.0 / cam_.scale;
        cam_.y = 0.5 - win_h_ / 2.0 / cam_.scale;
    }
    history_.push_back(node_to_json(*root_, true).dump());
    history_pos_ = 0;
}

App::~App() { flush(); }

void App::flush() {
    try {
        store::save(*root_, dir_);
        store::save_view(dir_, {anchor_->id, cam_, true});
        dirty_ = false;
        save_status_ = "Saved";
    } catch (const std::exception& e) {
        save_status_ = "Save failed";
        toast(std::string("Save failed: ") + e.what());
    }
}

void App::mark_dirty() {
    dirty_ = true;
    last_change_ = now_;
    save_status_ = "Saving…";
}

void App::toast(const std::string& msg) {
    toast_ = msg;
    toast_until_ = now_ + 2.5;
}

void App::commit() {
    update_bindings();
    std::string state = node_to_json(*root_, true).dump();
    if (history_pos_ >= 0 && history_[history_pos_] == state) {
        return;
    }
    history_.resize(history_pos_ + 1);
    history_.push_back(std::move(state));
    // ponytail: whole-map snapshots; switch to diffs if maps get huge.
    if (history_.size() > 200) {
        history_.erase(history_.begin());
    }
    history_pos_ = int(history_.size()) - 1;
    mark_dirty();
}

void App::restore(const std::string& state) {
    std::string anchor_id = anchor_->id;
    editing_ = nullptr;
    SDL_StopTextInput(window_);
    drag_ = Drag::None;
    creating_ = nullptr;
    erasing_.clear();
    selection_.clear();
    root_ = node_from_json(json::parse(state));
    anchor_ = find_by_id(*root_, anchor_id);
    if (!anchor_) {
        anchor_ = root_.get();
        cam_.scale = initial_root_px;
        cam_.x = 0.5 - win_w_ / 2.0 / cam_.scale;
        cam_.y = 0.5 - win_h_ / 2.0 / cam_.scale;
    }
    mark_dirty();
}

void App::undo() {
    finish_text_edit();
    if (history_pos_ > 0) {
        restore(history_[--history_pos_]);
    }
}

void App::redo() {
    if (history_pos_ + 1 < int(history_.size())) {
        restore(history_[++history_pos_]);
    }
}

void App::set_tool(Tool t) {
    finish_text_edit();
    tool_ = t;
    if (t != Tool::Select) {
        selection_.clear();
    }
    if (t == Tool::Image) {
        open_file_dialog();
        tool_ = Tool::Select;
    }
}

// ---------------------------------------------------------------- geometry

// The highest ancestor of the anchor that's still a sane size on screen.
// Drawing and picking start there, so shapes overlapping the anchor stay
// visible.
std::pair<Node*, Xform> App::draw_root() const {
    Xform t = cam_.xform();
    Node* n = anchor_;
    while (n->parent && parent_xform(*n, t).s <= draw_root_max) {
        t = parent_xform(*n, t);
        n = n->parent;
    }
    return {n, t};
}

std::optional<Xform> App::xform_of(const Node* n) const {
    // Walk up until we reach the anchor or one of its ancestors...
    std::vector<const Node*> chain;
    const Node* p = n;
    while (p && !is_ancestor(p, anchor_)) {
        chain.push_back(p);
        p = p->parent;
    }
    if (!p) {
        return std::nullopt;
    }
    // ...find that node's placement by climbing up from the anchor...
    Xform t = cam_.xform();
    for (const Node* q = anchor_; q != p; q = q->parent) {
        t = parent_xform(*q, t);
        if (t.s > max_scale) {
            return std::nullopt; // astronomically large: not usable
        }
    }
    // ...then walk back down to `n`.
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        t = child_xform(t, **it);
    }
    return t;
}

std::optional<Rect> App::screen_rect_of(const Node* n) const {
    auto t = xform_of(n);
    if (!t) {
        return std::nullopt;
    }
    return screen_rect(*n, *t);
}

// Keeps the anchor = the deepest node covering the whole window, with the
// camera in its units. This is what makes zoom depth unlimited.
void App::reroot() {
    // A later (higher) sibling that also fills the window is what you see,
    // so step up and let the downward pass pick it.
    auto hidden_by_sibling = [&] {
        if (!anchor_->parent) {
            return false;
        }
        Xform pt = parent_xform(*anchor_, cam_.xform());
        auto& sib = anchor_->parent->children;
        for (size_t i = size_t(index_in_parent(*anchor_)) + 1; i < sib.size(); ++i) {
            if (covers(screen_rect(*sib[i], child_xform(pt, *sib[i])), win_w_, win_h_)) {
                return true;
            }
        }
        return false;
    };
    while (anchor_->parent &&
           (!covers(screen_rect(*anchor_, cam_.xform()), win_w_, win_h_) ||
            hidden_by_sibling())) {
        const Node& a = *anchor_;
        cam_.x = cam_.x * a.w + a.x;
        cam_.y = cam_.y * a.w + a.y;
        cam_.scale /= a.w;
        anchor_ = a.parent;
    }
    bool moved = true;
    while (moved) {
        moved = false;
        Xform t = cam_.xform();
        // Topmost child first: that's the one you see.
        for (auto it = anchor_->children.rbegin(); it != anchor_->children.rend(); ++it) {
            const Node& c = **it;
            if (covers(screen_rect(c, child_xform(t, c)), win_w_, win_h_)) {
                cam_.x = (cam_.x - c.x) / c.w;
                cam_.y = (cam_.y - c.y) / c.w;
                cam_.scale *= c.w;
                anchor_ = it->get();
                moved = true;
                break;
            }
        }
    }
}

void App::clamp_zoom() {
    Vec2 center{win_w_ / 2.0, win_h_ / 2.0};
    if (anchor_ == root_.get()) {
        double min_scale = std::max(win_w_, win_h_);
        if (cam_.scale < min_scale) {
            cam_.zoom_at(center, min_scale / cam_.scale);
        }
    }
    if (cam_.scale > max_scale) {
        cam_.zoom_at(mouse_, max_scale / cam_.scale);
    }
}

bool App::on_label(const Node& n, Vec2 p) const {
    auto t = xform_of(&n);
    if (!t || n.text.empty()) {
        return false;
    }
    TextLayout L = layout_text(vg_, n, *t);
    for (size_t i = 0; i < L.lines.size(); ++i) {
        const auto& sp = L.lines[i];
        double w = text::measure(vg_, text::Hand,
                                 n.text.substr(sp.begin, sp.end - sp.begin), L.px);
        double h = L.px * text::line_height;
        Rect line{L.line_x[i], L.top + i * h, w, h};
        if (line.expanded(6).contains(p)) {
            return true;
        }
    }
    return false;
}

bool App::hit_self(const Node& n, Xform t, Vec2 p) const {
    Rect r = screen_rect(n, t);
    double sw = n.style.stroke_width * parent_scale(n, t);
    double tol = std::max(6.0, sw / 2 + 4);
    // Titles on the border and arrow labels belong to their shape.
    if (!n.text.empty() && n.kind != Kind::Text && n.kind != Kind::Code &&
        (n.is_linear() || !n.children.empty()) && on_label(n, p)) {
        return true;
    }
    bool filled = (n.style.fill & 0xff) != 0 ||
                  (!n.text.empty() && n.children.empty());
    switch (n.kind) {
    case Kind::Rect:
        if (filled) {
            return r.contains(p);
        }
        return r.expanded(tol).contains(p) && !r.expanded(-tol).contains(p);
    case Kind::Ellipse:
    case Kind::Diamond: {
        Vec2 c = r.center();
        double rx = r.w / 2, ry = r.h / 2;
        double dx = (p.x - c.x) / rx, dy = (p.y - c.y) / ry;
        double d = n.kind == Kind::Ellipse ? std::sqrt(dx * dx + dy * dy)
                                           : std::abs(dx) + std::abs(dy);
        if (filled && d <= 1) {
            return true;
        }
        return std::abs(d - 1) * std::min(rx, ry) <= tol;
    }
    case Kind::Line:
    case Kind::Arrow:
    case Kind::Draw: {
        auto pts = screen_points(n, t);
        if (pts.size() == 1) {
            return length(p - pts[0]) <= tol;
        }
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            if (dist_to_segment(p, pts[i], pts[i + 1]) <= tol) {
                return true;
            }
        }
        return false;
    }
    default:
        return r.contains(p);
    }
}

Node* App::pick_in(Node& n, Xform t, Vec2 p) {
    Rect r = screen_rect(n, t);
    double sw = n.style.stroke_width * parent_scale(n, t);
    // Labels can reach past the box: a title on the top border, a long
    // label on a short arrow.
    double label = n.text.empty() || n.kind == Kind::Text ? 0
                   : n.style.font_size * parent_scale(n, t) * (n.is_linear() ? 8 : 1.5);
    if (n.kind != Kind::Root &&
        (!r.expanded(std::max({8.0, sw, label})).contains(p) || (r.w < 2 && r.h < 2))) {
        return nullptr;
    }
    // Topmost first: later siblings, and children above their parent.
    for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
        if (Node* hit = pick_in(**it, child_xform(t, **it), p)) {
            return hit;
        }
    }
    if (n.kind == Kind::Root || covers(r, win_w_, win_h_)) {
        return nullptr; // the background you're zoomed into
    }
    if (hit_self(n, t, p)) {
        return n.kind == Kind::PdfPage ? n.parent : &n;
    }
    return nullptr;
}

Node* App::pick(Vec2 p) {
    auto [n, t] = draw_root();
    return pick_in(*n, t, p);
}

// Deepest container under `p` (where a new shape at `p` should live).
Node* App::container_at(Vec2 p, const Node* exclude) {
    Node* best = anchor_;
    while (!best->is_container()) {
        best = best->parent;
    }
    Xform t = *xform_of(best);
    while (true) {
        Node* next = nullptr;
        for (auto it = best->children.rbegin(); it != best->children.rend(); ++it) {
            Node& c = **it;
            if (!c.is_container() || c.kind == Kind::PdfPage ||
                (exclude && is_ancestor(exclude, &c))) {
                continue;
            }
            Xform ct = child_xform(t, c);
            Rect r = screen_rect(c, ct);
            if (r.w < 40 || !r.contains(p)) {
                continue;
            }
            bool inside = true;
            if (c.kind == Kind::Ellipse || c.kind == Kind::Diamond) {
                Vec2 cc = r.center();
                double dx = (p.x - cc.x) / (r.w / 2), dy = (p.y - cc.y) / (r.h / 2);
                inside = c.kind == Kind::Ellipse ? dx * dx + dy * dy <= 1
                                                 : std::abs(dx) + std::abs(dy) <= 1;
            }
            if (inside) {
                next = &c;
                t = ct;
                break;
            }
        }
        if (!next) {
            return best;
        }
        best = next;
    }
}

void App::reparent(Node& n, Node& np) {
    auto old_t = xform_of(n.parent);
    auto new_t = xform_of(&np);
    auto r = screen_rect_of(&n);
    if (!old_t || !new_t || !r) {
        return;
    }
    n.x = (r->x - new_t->ox) / new_t->s;
    n.y = (r->y - new_t->oy) / new_t->s;
    n.w = r->w / new_t->s;
    n.h = r->h / new_t->s;
    double k = old_t->s / new_t->s;
    n.style.stroke_width *= k;
    n.style.font_size *= k;
    add_child(np, detach(n));
}

void App::set_linear_points(Node& n, const std::vector<Vec2>& pts) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (Vec2 p : pts) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    double pad = std::max(n.style.stroke_width / 2, 1e-300);
    n.x = x0 - pad;
    n.y = y0 - pad;
    n.w = x1 - x0 + 2 * pad;
    n.h = y1 - y0 + 2 * pad;
    n.points.clear();
    for (Vec2 p : pts) {
        n.points.push_back({(p.x - n.x) / n.w, (p.y - n.y) / n.h});
    }
}

std::vector<Vec2> App::linear_local_points(const Node& n) const {
    std::vector<Vec2> out;
    for (Vec2 p : n.points) {
        out.push_back({n.x + p.x * n.w, n.y + p.y * n.h});
    }
    return out;
}

// ---------------------------------------------------------------- editing

void App::place_new_node(Node& parent, Node& n, Rect r) {
    auto tp = xform_of(&parent);
    if (!tp) {
        return;
    }
    n.x = (r.x - tp->ox) / tp->s;
    n.y = (r.y - tp->oy) / tp->s;
    n.w = r.w / tp->s;
    n.h = r.h / tp->s;
}

void App::auto_color(Node& n) {
    int used[ui::n_hues] = {};
    for (const auto& c : n.parent->children) {
        if (c.get() != &n) {
            if (int h = ui::hue_of(c->style.fill); h >= 0) {
                ++used[h];
            }
        }
    }
    if (int ph = ui::hue_of(n.parent->style.fill); ph >= 0) {
        used[ph] += 1000; // never the parent's own color
    }
    int best = ui::auto_order[0];
    for (int h : ui::auto_order) {
        if (used[h] < used[best]) {
            best = h;
        }
    }
    n.style.fill = ui::shade(best, ui::auto_fill_shade);
    n.style.stroke = ui::shade(best, ui::auto_stroke_shade);
}

void App::start_create(Vec2 p, Mods) {
    Node* parent = container_at(p, nullptr);
    auto tp = xform_of(parent);
    if (!tp) {
        return;
    }
    auto node = make_node(kind_for(tool_));
    node->style = style_;
    node->style.stroke_width = stroke_px_ / tp->s;
    node->style.font_size = font_px_ / tp->s;
    if (node->is_linear()) {
        node->style.fill = 0;
    }
    create_start_local_ = tp->to_local(p);
    node->x = create_start_local_.x;
    node->y = create_start_local_.y;
    node->w = node->h = 1 / tp->s;
    creating_ = &add_child(*parent, std::move(node));
    if (auto_color_ && (creating_->kind == Kind::Rect || creating_->kind == Kind::Ellipse ||
                        creating_->kind == Kind::Diamond)) {
        auto_color(*creating_);
    }
    if (creating_->is_linear()) {
        draw_local_ = {create_start_local_};
        set_linear_points(*creating_, {create_start_local_, create_start_local_});
    }
    drag_ = Drag::Create;
}

void App::update_create(Vec2 p, Mods m) {
    auto tp = xform_of(creating_->parent);
    if (!tp) {
        return;
    }
    Vec2 a = create_start_local_, q = tp->to_local(p);
    switch (creating_->kind) {
    case Kind::Line:
    case Kind::Arrow:
        if (m.shift) { // snap to 15 degrees
            Vec2 d = q - a;
            double ang = std::round(std::atan2(d.y, d.x) / (M_PI / 12)) * (M_PI / 12);
            double len = length(d);
            q = {a.x + std::cos(ang) * len, a.y + std::sin(ang) * len};
        }
        set_linear_points(*creating_, {a, q});
        break;
    case Kind::Draw:
        if (length(q - draw_local_.back()) * tp->s > 1.5) {
            draw_local_.push_back(q);
            set_linear_points(*creating_, draw_local_);
        }
        break;
    default: {
        if (m.shift) {
            double d = std::max(std::abs(q.x - a.x), std::abs(q.y - a.y));
            q = {a.x + std::copysign(d, q.x - a.x), a.y + std::copysign(d, q.y - a.y)};
        }
        Rect r = Rect::from_points(a, q);
        creating_->x = r.x;
        creating_->y = r.y;
        creating_->w = std::max(r.w, 1 / tp->s);
        creating_->h = std::max(r.h, 1 / tp->s);
        break;
    }
    }
}

void App::finish_create() {
    Node* n = creating_;
    creating_ = nullptr;
    auto tp = xform_of(n->parent);
    auto r = screen_rect_of(n);
    bool tiny = !tp || !r || (r->w < 4 && r->h < 4);
    if (n->kind == Kind::Draw && tp) {
        tiny = false; // a click with the pen is a dot
        std::vector<Vec2> simple;
        simplify(draw_local_, 0.5 / tp->s, simple);
        set_linear_points(*n, simple);
    }
    if (tiny) {
        detach(*n);
        return;
    }
    if (n->kind == Kind::Line || n->kind == Kind::Arrow) {
        // A connector belongs to the deepest shape that holds both its ends.
        auto t = xform_of(n);
        auto pts = t ? screen_points(*n, *t) : std::vector<Vec2>{};
        if (pts.size() >= 2) {
            Node* pa = container_at(pts.front(), n);
            Node* pb = container_at(pts.back(), n);
            while (!is_ancestor(pa, pb)) {
                pa = pa->parent;
            }
            if (pa != n->parent) {
                reparent(*n, *pa);
            }
        }
    }
    bind_endpoints(*n);
    if (tool_ != Tool::Draw) {
        tool_ = Tool::Select;
        selection_ = {n};
    }
    commit();
}

void App::start_text_edit(Node& n, bool) {
    finish_text_edit();
    if (n.style.font_size <= 0) {
        auto tp = xform_of(n.parent);
        n.style.font_size = font_px_ / (tp ? tp->s : 1);
    }
    if (n.kind == Kind::Text && n.w < 12 * n.style.font_size) {
        n.w = note::default_width * n.style.font_size; // notes from before page width
    }
    editing_ = &n;
    caret_ = n.text.size();
    vim_.begin(n.text, caret_);
    caret_blink_start_ = now_;
    selection_ = {&n};
    SDL_StartTextInput(window_);
}

void App::finish_text_edit() {
    if (!editing_) {
        return;
    }
    Node* n = editing_;
    editing_ = nullptr;
    SDL_StopTextInput(window_);
    if (n->kind != Kind::Code && !n->is_linear()) {
        split_fences(*n);
    }
    if (n->kind == Kind::Text) {
        fit_text_bounds(vg_, *n);
    }
    bool blank = n->text.find_first_not_of(" \n\t") == std::string::npos;
    if (n->kind == Kind::Text && blank) {
        std::erase(selection_, n);
        detach(*n);
    }
    commit();
}

std::vector<Node*> App::top_level_selection() const {
    std::vector<Node*> out;
    for (Node* n : selection_) {
        bool nested = false;
        for (Node* other : selection_) {
            if (other != n && is_ancestor(other, n)) {
                nested = true;
            }
        }
        if (!nested) {
            out.push_back(n);
        }
    }
    return out;
}

void App::delete_selection() {
    for (Node* n : top_level_selection()) {
        if (is_ancestor(n, anchor_)) {
            continue; // never delete the place you're standing in
        }
        detach(*n);
    }
    selection_.clear();
    commit();
}

void App::copy_selection() {
    json items = json::array();
    std::string plain;
    for (Node* n : top_level_selection()) {
        auto r = screen_rect_of(n);
        auto tp = xform_of(n->parent);
        if (!r || !tp) {
            continue;
        }
        items.push_back({{"node", node_to_json(*n, true)},
                         {"rect", {r->x, r->y, r->w, r->h}},
                         {"s", tp->s}});
        if (!n->text.empty()) {
            plain += (plain.empty() ? "" : "\n") + n->text;
        }
    }
    if (items.empty()) {
        return;
    }
    clipboard_ = items.dump();
    clipboard_text_ = plain.empty() ? "[tanuki shapes]" : plain;
    SDL_SetClipboardText(clipboard_text_.c_str());
}

void App::paste(Vec2 at) {
    if (SDL_HasClipboardData("image/png")) {
        size_t size = 0;
        void* data = SDL_GetClipboardData("image/png", &size);
        if (data && size) {
            std::string rel = assets_.import_bytes(data, size, ".png");
            SDL_free(data);
            insert_file((dir_ / rel).string(), at);
            fs::remove(dir_ / rel); // insert_file copied it again
        }
        return;
    }
    char* raw = SDL_GetClipboardText();
    std::string txt = raw ? raw : "";
    SDL_free(raw);

    Node* parent = container_at(at, nullptr);
    auto tp = xform_of(parent);
    if (!tp) {
        return;
    }
    if (!clipboard_.empty() && txt == clipboard_text_) {
        json items = json::parse(clipboard_);
        Rect bbox{1e300, 1e300, 0, 0};
        double x1 = -1e300, y1 = -1e300;
        for (const auto& it : items) {
            bbox.x = std::min(bbox.x, it["rect"][0].get<double>());
            bbox.y = std::min(bbox.y, it["rect"][1].get<double>());
            x1 = std::max(x1, it["rect"][0].get<double>() + it["rect"][2].get<double>());
            y1 = std::max(y1, it["rect"][1].get<double>() + it["rect"][3].get<double>());
        }
        bbox.w = x1 - bbox.x;
        bbox.h = y1 - bbox.y;
        Vec2 shift = at - bbox.center();
        selection_.clear();
        std::map<std::string, std::string> remap;
        std::vector<std::unique_ptr<Node>> fresh;
        for (const auto& it : items) {
            fresh.push_back(node_from_json(it["node"]));
            regen_ids(*fresh.back(), remap);
        }
        for (auto& n : fresh) {
            remap_binds(*n, remap);
        }
        size_t idx = 0;
        for (const auto& it : items) {
            auto n = std::move(fresh[idx++]);
            Rect r{it["rect"][0].get<double>() + shift.x,
                   it["rect"][1].get<double>() + shift.y,
                   it["rect"][2].get<double>(), it["rect"][3].get<double>()};
            double k = it["s"].get<double>() / tp->s;
            n->style.stroke_width *= k;
            n->style.font_size *= k;
            Node& added = add_child(*parent, std::move(n));
            place_new_node(*parent, added, r);
            selection_.push_back(&added);
        }
        commit();
        return;
    }
    if (!txt.empty()) {
        auto n = make_node(Kind::Text);
        n->style = style_;
        n->style.font_size = font_px_ / tp->s;
        n->text = txt;
        fit_text_bounds(vg_, *n);
        Vec2 q = tp->to_local(at);
        n->x = q.x - n->w / 2;
        n->y = q.y - n->h / 2;
        selection_ = {&add_child(*parent, std::move(n))};
        commit();
    }
}

void App::duplicate_selection() {
    std::vector<Node*> copies;
    std::map<std::string, std::string> remap;
    for (Node* n : top_level_selection()) {
        auto tp = xform_of(n->parent);
        if (!tp || is_ancestor(n, anchor_)) {
            continue;
        }
        auto c = node_from_json(node_to_json(*n, true));
        regen_ids(*c, remap);
        c->x += 12 / tp->s;
        c->y += 12 / tp->s;
        copies.push_back(&add_child(*n->parent, std::move(c)));
    }
    for (Node* c : copies) {
        remap_binds(*c, remap);
    }
    if (!copies.empty()) {
        selection_ = copies;
        commit();
    }
}

void App::select_all() {
    selection_.clear();
    for (auto& c : anchor_->children) {
        selection_.push_back(c.get());
    }
}

void App::move_layer(bool to_front) {
    for (Node* n : top_level_selection()) {
        Node* p = n->parent;
        auto owned = detach(*n);
        owned->parent = p;
        if (to_front) {
            p->children.push_back(std::move(owned));
        } else {
            p->children.insert(p->children.begin(), std::move(owned));
        }
    }
    commit();
}

void App::nudge(double dx, double dy) {
    for (Node* n : top_level_selection()) {
        if (auto tp = xform_of(n->parent)) {
            n->x += dx / tp->s;
            n->y += dy / tp->s;
        }
    }
    commit();
}

void App::apply_to_selection(const std::function<void(Node&)>& f) {
    for (Node* n : selection_) {
        f(*n);
    }
    if (!selection_.empty()) {
        commit();
    }
}

// ---------------------------------------------------------------- files

void App::open_file_dialog() {
    static const SDL_DialogFileFilter filters[] = {
        {"Images and PDFs", "png;jpg;jpeg;gif;bmp;tga;pdf"}};
    SDL_ShowOpenFileDialog(
        [](void* self, const char* const* files, int) {
            if (!files) {
                return;
            }
            App* app = static_cast<App*>(self);
            std::lock_guard lock(app->pending_mutex_);
            for (int i = 0; files[i]; ++i) {
                app->pending_files_.push_back(files[i]);
            }
        },
        this, window_, filters, 1, nullptr, true);
}

void App::drop_file(const std::string& path, Vec2 p) { insert_file(path, p); }

void App::insert_file(const std::string& path, Vec2 center) {
    std::string ext = fs::path(path).extension().string();
    for (char& c : ext) {
        c = char(std::tolower((unsigned char)c));
    }
    Node* parent = container_at(center, nullptr);
    auto tp = xform_of(parent);
    if (!tp) {
        return;
    }
    std::string rel;
    try {
        rel = assets_.import(path);
    } catch (const std::exception& e) {
        toast(std::string("Couldn't import: ") + e.what());
        return;
    }

    if (ext == ".pdf") {
        int pages = assets_.pdf_page_count(rel);
        if (pages == 0) {
            toast("Couldn't open PDF");
            return;
        }
        auto pdf = make_node(Kind::Pdf);
        pdf->text = fs::path(path).stem().string();
        pdf->style.stroke_width = 0;
        // Pages stacked vertically in the container's local units.
        double y = 0.08;
        for (int i = 0; i < pages; ++i) {
            auto [pw, ph] = assets_.pdf_page_size(rel, i);
            auto page = make_node(Kind::PdfPage);
            page->asset = rel;
            page->page = i;
            page->style.stroke_width = 0;
            page->x = 0.05;
            page->y = y;
            page->w = 0.9;
            page->h = pw > 0 ? 0.9 * ph / pw : 0.9 * 1.414;
            y += page->h + 0.03;
            add_child(*pdf, std::move(page));
        }
        double aspect = y + 0.02;
        double w_px = std::min(win_w_ * 0.4, 600.0);
        double h_px = w_px * aspect;
        Rect r{center.x - w_px / 2, center.y - std::min(h_px, win_h_ * 0.4), w_px, h_px};
        Node& added = add_child(*parent, std::move(pdf));
        place_new_node(*parent, added, r);
        added.style.font_size = added.w * 0.035;
        selection_ = {&added};
        commit();
        return;
    }

    const Texture* tex = assets_.image(rel);
    if (!tex) {
        fs::remove(dir_ / rel);
        toast("Unsupported file: " + fs::path(path).filename().string());
        return;
    }
    double k = std::min({1.0, win_w_ * 0.5 / tex->w, win_h_ * 0.5 / tex->h});
    double w = tex->w * k, h = tex->h * k;
    auto img = make_node(Kind::Image);
    img->asset = rel;
    img->style.stroke_width = 0;
    Node& added = add_child(*parent, std::move(img));
    place_new_node(*parent, added, {center.x - w / 2, center.y - h / 2, w, h});
    selection_ = {&added};
    commit();
}

// ---------------------------------------------------------------- editing

int App::raw_line() const {
    return editing_ && editing_->kind == Kind::Text ? note::line_of(editing_->text, caret_)
                                                    : -1;
}

void App::refit_editing() {
    if (editing_ && editing_->kind == Kind::Text) {
        fit_text_bounds(vg_, *editing_, raw_line());
    } else if (editing_ && editing_->kind == Kind::Code) {
        fit_code_bounds(vg_, *editing_);
    }
}

// Removes the line the caret is on, with the line break before it.
static void erase_caret_line(std::string& s, size_t& caret) {
    size_t ls = line_start(s, caret);
    size_t le = s.find('\n', caret);
    le = le == std::string::npos ? s.size() : le;
    size_t a = ls > 0 ? ls - 1 : ls;
    s.erase(a, le - a);
    caret = a;
}

void App::open_code_block(const std::string& tag) {
    Node* note = editing_;
    erase_caret_line(note->text, caret_);
    std::string lang = code::from_fence(tag);
    code_lang_ = lang;
    double fs = note->style.font_size * 0.8;
    if (note->kind == Kind::Text &&
        note->text.find_first_not_of(" \n\t") == std::string::npos) {
        // The note was just the fence: it becomes the code block.
        note->kind = Kind::Code;
        note->lang = lang;
        note->text.clear();
        note->style.font_size = fs;
        caret_ = 0;
        fit_code_bounds(vg_, *note);
        vim_.begin(note->text, 0);
        return;
    }
    if (note->kind == Kind::Text) {
        fit_text_bounds(vg_, *note);
    }
    auto c = make_node(Kind::Code);
    c->lang = lang;
    c->style = note->style;
    c->style.font_size = fs;
    fit_code_bounds(vg_, *c);
    c->x = note->x;
    c->y = note->y + note->h + note->style.font_size * 0.5;
    Node& added = add_child(*note->parent, std::move(c));
    start_text_edit(added);
}

void App::split_fences(Node& n) {
    std::string& s = n.text;
    // [start, end) of the first line at or after `from` that starts with ```.
    auto fence_line = [&](size_t from) -> std::pair<size_t, size_t> {
        for (size_t ls = from; ls <= s.size();) {
            size_t le = std::min(s.find('\n', ls), s.size());
            size_t a = s.find_first_not_of(" \t", ls);
            if (a < le && s.compare(a, 3, "```") == 0) {
                return {ls, le};
            }
            ls = le + 1;
        }
        return {std::string::npos, std::string::npos};
    };
    auto [o1, o2] = fence_line(0);
    if (o1 == std::string::npos || o2 >= s.size()) {
        return;
    }
    auto [c1, c2] = fence_line(o2 + 1);
    if (c1 == std::string::npos) {
        return; // not closed yet
    }
    std::string open = s.substr(o1, o2 - o1);
    std::string lang = code::from_fence(open.substr(open.find("```") + 3));
    std::string body = c1 > o2 + 1 ? s.substr(o2 + 1, c1 - 1 - (o2 + 1)) : "";
    std::string after = c2 < s.size() ? s.substr(c2 + 1) : "";
    std::string before = s.substr(0, o1);
    auto blank = [](const std::string& t) {
        return t.find_first_not_of(" \n\t") == std::string::npos;
    };
    while (!before.empty() && (before.back() == '\n' || before.back() == ' ')) {
        before.pop_back();
    }
    after.erase(0, std::min(after.find_first_not_of('\n'), after.size()));

    double fs = n.style.font_size;
    Node* block;
    if (n.kind == Kind::Text && blank(before)) {
        n.kind = Kind::Code;
        n.lang = lang;
        n.text = body;
        n.style.font_size = fs * 0.8;
        fit_code_bounds(vg_, n);
        block = &n;
    } else {
        n.text = before;
        if (n.kind == Kind::Text) {
            fit_text_bounds(vg_, n);
        }
        auto c = make_node(Kind::Code);
        c->lang = lang;
        c->text = body;
        c->style = n.style;
        c->style.font_size = fs * 0.8;
        fit_code_bounds(vg_, *c);
        c->x = n.x;
        c->y = n.y + n.h + fs * 0.5;
        block = &add_child(*n.parent, std::move(c));
    }
    if (!blank(after)) {
        auto t = make_node(Kind::Text);
        t->style = block->style;
        t->style.font_size = fs;
        t->text = after;
        fit_text_bounds(vg_, *t);
        t->x = block->x;
        t->y = block->y + block->h + fs * 0.5;
        split_fences(add_child(*block->parent, std::move(t)));
    }
}

void App::close_code_block() {
    Node* block = editing_;
    erase_caret_line(block->text, caret_);
    fit_code_bounds(vg_, *block);
    // Carry on writing in a new note below (dropped again if left empty).
    auto n = make_node(Kind::Text);
    n->style = block->style;
    n->style.font_size = block->style.font_size / 0.8;
    n->x = block->x;
    n->y = block->y + block->h + n->style.font_size * 0.5;
    fit_text_bounds(vg_, *n);
    Node& added = add_child(*block->parent, std::move(n));
    start_text_edit(added);
}

// The title a [[link]] matches: a note's or label's first line, or a file name.
static std::string link_title(const Node& n) {
    if (n.kind == Kind::Root || n.kind == Kind::Code || n.kind == Kind::Line ||
        n.kind == Kind::Arrow || n.kind == Kind::Draw || n.kind == Kind::PdfPage) {
        return "";
    }
    std::string first = n.text.substr(0, n.text.find('\n'));
    while (!first.empty() && first.back() == ' ') {
        first.pop_back();
    }
    if (!first.empty()) {
        return first;
    }
    return n.asset.empty() ? "" : display_name(n);
}

static std::string lower_ascii(std::string s) {
    for (char& c : s) {
        c = char(std::tolower((unsigned char)c));
    }
    return s;
}

std::optional<std::string> App::link_at(Vec2 p) {
    Node* n = pick(p);
    auto t = n ? xform_of(n) : std::nullopt;
    if (!t || n->kind == Kind::Code || n->text.find("[[") == std::string::npos) {
        return std::nullopt;
    }
    if (n->kind == Kind::Text) {
        double em = n->style.font_size * t->s / n->w;
        auto L = note::layout(vg_, n->text, n->w / n->style.font_size, -1);
        for (const auto& pc : L->pieces) {
            const note::Line& line = L->lines[size_t(pc.line)];
            Rect r{t->ox + pc.x * em, t->oy + line.y * em, pc.w * em, line.h * em};
            if (pc.flags & note::Link && r.contains(p)) {
                size_t a = n->text.rfind("[[", pc.begin), b = n->text.find("]]", pc.begin);
                if (a != std::string::npos && b != std::string::npos) {
                    return n->text.substr(a + 2, b - a - 2);
                }
            }
        }
        return std::nullopt;
    }
    TextLayout L = layout_text(vg_, *n, *t);
    for (size_t i = 0; i < L.lines.size(); ++i) {
        std::string line = n->text.substr(L.lines[i].begin, L.lines[i].end - L.lines[i].begin);
        double y = L.top + i * L.px * text::line_height;
        if (p.y < y || p.y > y + L.px * text::line_height) {
            continue;
        }
        for (const auto& l : link_spans(line)) {
            double x0 = L.line_x[i] + text::measure(vg_, text::Hand, line.substr(0, l.begin), L.px);
            double x1 = L.line_x[i] + text::measure(vg_, text::Hand, line.substr(0, l.end), L.px);
            if (p.x >= x0 && p.x <= x1) {
                return line.substr(l.begin + 2, l.end - l.begin - 4);
            }
        }
    }
    return std::nullopt;
}

void App::follow_link(const std::string& title) {
    std::string want = lower_ascii(title);
    // Shallowest match first.
    std::vector<Node*> level{root_.get()};
    while (!level.empty()) {
        std::vector<Node*> next;
        for (Node* n : level) {
            if (lower_ascii(link_title(*n)) == want) {
                selection_ = {n};
                fly_to(n);
                return;
            }
            for (auto& c : n->children) {
                next.push_back(c.get());
            }
        }
        level.swap(next);
    }
    toast("No note called \u201c" + title + "\u201d");
}

std::optional<std::pair<size_t, std::string>> App::link_query() const {
    if (!editing_ || editing_->kind == Kind::Code || vim_.mode != vim::Mode::Insert) {
        return std::nullopt;
    }
    const std::string& s = editing_->text;
    size_t open = caret_ < 2 ? std::string::npos : s.rfind("[[", caret_ - 2);
    if (open == std::string::npos) {
        return std::nullopt;
    }
    std::string q = s.substr(open + 2, caret_ - open - 2);
    if (q.find('\n') != std::string::npos || q.find("]]") != std::string::npos) {
        return std::nullopt;
    }
    return std::pair{open, q};
}

std::vector<std::string> App::link_matches(const std::string& query) const {
    std::string q = lower_ascii(query);
    std::vector<std::pair<int, std::string>> found;
    std::set<std::string> seen;
    std::function<void(const Node&)> walk = [&](const Node& n) {
        std::string title = link_title(n);
        if (&n != editing_ && !title.empty() && !seen.contains(title)) {
            std::string lt = lower_ascii(title);
            size_t at = lt.find(q);
            if (at != std::string::npos) {
                bool word = at == 0 || lt[at - 1] == ' ';
                found.push_back({at == 0 ? 0 : word ? 1 : 2, title});
                seen.insert(title);
            }
        }
        for (const auto& c : n.children) {
            walk(*c);
        }
    };
    walk(*root_);
    std::ranges::sort(found, [](auto& a, auto& b) {
        return a.first != b.first ? a.first < b.first : a.second.size() < b.second.size();
    });
    std::vector<std::string> out;
    for (size_t i = 0; i < found.size() && i < 8; ++i) {
        out.push_back(found[i].second);
    }
    return out;
}

void App::accept_link() {
    auto q = link_query();
    auto matches = q ? link_matches(q->second) : std::vector<std::string>{};
    if (matches.empty()) {
        return;
    }
    const std::string& title = matches[size_t(std::clamp(link_sel_, 0, int(matches.size()) - 1))];
    std::string& s = editing_->text;
    size_t start = q->first + 2;
    std::string add = title + "]]";
    s.replace(start, caret_ - start, add);
    caret_ = start + add.size();
    refit_editing();
}

void App::draw_link_popup() {
    auto q = link_query();
    auto t = editing_ ? xform_of(editing_) : std::nullopt;
    if (!q || !t) {
        return;
    }
    if (q->second != link_last_query_) {
        link_last_query_ = q->second;
        link_sel_ = 0;
    }
    auto matches = link_matches(q->second);
    if (matches.empty()) {
        return;
    }
    link_sel_ = std::clamp(link_sel_, 0, int(matches.size()) - 1);
    CaretBox c = caret_box(vg_, *editing_, *t, caret_, raw_line());
    double w = 280, row = 30;
    Rect panel{std::clamp(c.x, 8.0, win_w_ - w - 8), c.y + c.h + 6, w,
               8 + matches.size() * row};
    if (panel.bottom() > win_h_ - 8) {
        panel.y = c.y - panel.h - 6;
    }
    ui::island(vg_, panel);
    for (size_t i = 0; i < matches.size(); ++i) {
        Rect r{panel.x + 4, panel.y + 4 + i * row, w - 8, row};
        ui::button_bg(vg_, r, int(i) == link_sel_, false);
        ui::label(vg_, matches[i], r.x + 10, r.center().y, 13.5f, ui::rgb(ui::ink));
    }
}

void App::edit_selected() {
    if (selection_.size() != 1) {
        return;
    }
    Node& n = *selection_[0];
    if (n.kind == Kind::Text || n.kind == Kind::Code || n.kind == Kind::Rect ||
        n.kind == Kind::Ellipse || n.kind == Kind::Diamond || n.kind == Kind::Line ||
        n.kind == Kind::Arrow) {
        start_text_edit(n);
    }
}

// ---------------------------------------------------------------- navigation

void App::fly_to(const Node* n) {
    fly_id_ = n->id;
    fly_content_ = n->kind == Kind::Root;
    flying_ = true;
}

void App::fly_to_content() {
    fly_id_ = anchor_->id;
    fly_content_ = true;
    flying_ = true;
}

void App::fly_out() {
    // The shape we're "in": the deepest one under the middle of the screen
    // that fills most of it.
    Vec2 mid{win_w_ / 2.0, win_h_ / 2.0};
    Node* world = container_at(mid, nullptr);
    while (world->parent) {
        auto r = screen_rect_of(world);
        if (r && (r->w >= win_w_ * 0.6 || r->h >= win_h_ * 0.6)) {
            break;
        }
        world = world->parent;
    }
    if (!world->parent) {
        fly_to_content();
        return;
    }
    fly_to(world->parent);
    // Select the shape we left, so hjkl and Enter carry on from it.
    selection_ = {world};
    came_from_id_ = world->id;
}

void App::step_fly(double dt) {
    if (!flying_) {
        return;
    }
    Node* target = find_by_id(*root_, fly_id_);
    if (!target) {
        flying_ = false;
        return;
    }
    std::optional<Rect> T;
    if (fly_content_) {
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (auto& c : target->children) {
            if (auto r = screen_rect_of(c.get())) {
                x0 = std::min(x0, r->x);
                y0 = std::min(y0, r->y);
                x1 = std::max(x1, r->right());
                y1 = std::max(y1, r->bottom());
            }
        }
        if (x1 > x0) {
            T = Rect{x0, y0, x1 - x0, y1 - y0};
        } else if (target->children.empty()) {
            flying_ = false;
            return;
        }
    } else {
        T = screen_rect_of(target);
    }
    Vec2 center{win_w_ / 2.0, win_h_ / 2.0};
    if (!T) { // target is far above us: zoom out until it's reachable
        cam_.zoom_at(center, 0.5);
        return;
    }
    double margin = 0.85;
    double f = std::min(win_w_ * margin / std::max(T->w, 1e-9),
                        win_h_ * margin / std::max(T->h, 1e-9));
    Vec2 dc = center - T->center();
    if (std::abs(std::log(f)) < 0.003 && length(dc) < 0.5) {
        flying_ = false;
        return;
    }
    double k = 1 - std::exp(-dt * 9);
    // Zoom in log space (so many levels feel as smooth as one), centered on
    // the target, while sliding it to the middle of the window.
    cam_.zoom_at(T->center(), std::pow(f, k));
    cam_.pan(dc.x * k, dc.y * k);
}

// ---------------------------------------------------------------- attaching

// Topmost shape under `p` an arrow end can attach to. Never the arrow's own
// container (you'd attach to the box you're drawing inside).
Node* App::bind_target_at(Vec2 p, const Node* exclude) {
    Node* best = nullptr;
    std::function<void(Node&, Xform)> walk = [&](Node& n, Xform t) {
        for (auto& c : n.children) {
            Xform ct = child_xform(t, *c);
            Rect r = screen_rect(*c, ct);
            if (!r.expanded(10).contains(p)) {
                continue;
            }
            bool bindable = c->kind == Kind::Rect || c->kind == Kind::Ellipse ||
                            c->kind == Kind::Diamond || c->kind == Kind::Text ||
                            c->kind == Kind::Image || c->kind == Kind::Pdf;
            if (bindable && c.get() != exclude && !is_ancestor(c.get(), exclude) &&
                !covers(r, win_w_, win_h_) && r.w > 4) {
                best = c.get(); // later = drawn on top, so it wins
            }
            if (c->is_container()) {
                walk(*c, ct);
            }
        }
    };
    auto [root, t] = draw_root();
    walk(*root, t);
    return best;
}

void App::bind_endpoints(Node& n) {
    if (n.kind != Kind::Arrow && n.kind != Kind::Line) {
        return;
    }
    auto t = xform_of(&n);
    if (!t) {
        return;
    }
    auto pts = screen_points(n, *t);
    if (pts.size() < 2) {
        return;
    }
    Node* a = bind_target_at(pts.front(), &n);
    Node* b = bind_target_at(pts.back(), &n);
    n.start_bind = a ? a->id : "";
    n.end_bind = b ? b->id : "";
}

// Moves attached arrow ends onto the outline of their shapes.
// ponytail: walks the whole tree each frame; index bindings if maps get huge.
void App::update_bindings() {
    std::unordered_map<std::string, Node*> by_id;
    std::vector<Node*> linked;
    std::function<void(Node&)> walk = [&](Node& n) {
        by_id[n.id] = &n;
        if (!n.start_bind.empty() || !n.end_bind.empty()) {
            linked.push_back(&n);
        }
        for (auto& c : n.children) {
            walk(*c);
        }
    };
    walk(*root_);

    for (Node* n : linked) {
        auto tp = xform_of(n->parent);
        auto t = xform_of(n);
        if (!tp || !t || n->points.size() < 2) {
            continue;
        }
        auto target = [&](const std::string& id) -> std::optional<std::pair<Node*, Rect>> {
            auto it = by_id.find(id);
            if (id.empty() || it == by_id.end() || it->second == n) {
                return std::nullopt;
            }
            auto r = screen_rect_of(it->second);
            if (!r) {
                return std::nullopt;
            }
            return std::make_pair(it->second, *r);
        };
        auto a = target(n->start_bind), b = target(n->end_bind);
        if (!a && !b) {
            continue;
        }
        auto pts = screen_points(*n, *t);
        Vec2 p0 = pts.front(), p1 = pts.back();
        double gap = std::max(4.0, n->style.stroke_width * tp->s * 2);
        Vec2 aim0 = b ? b->second.center() : (pts.size() > 2 ? pts[1] : p1);
        Vec2 aim1 = a ? a->second.center() : (pts.size() > 2 ? pts[pts.size() - 2] : p0);
        if (a) {
            p0 = boundary_point(*a->first, a->second, aim0, gap).value_or(p0);
        }
        if (b) {
            p1 = boundary_point(*b->first, b->second, aim1, gap).value_or(p1);
        }
        if (length(p0 - pts.front()) < 0.01 && length(p1 - pts.back()) < 0.01) {
            continue;
        }
        pts.front() = p0;
        pts.back() = p1;
        std::vector<Vec2> local;
        for (Vec2 p : pts) {
            local.push_back(tp->to_local(p));
        }
        set_linear_points(*n, local);
    }
}

// ---------------------------------------------------------------- search

void App::open_search() {
    finish_text_edit();
    search_open_ = true;
    search_query_.clear();
    search_sel_ = 0;
    caret_blink_start_ = now_;
    SDL_StartTextInput(window_);
    run_search();
}

void App::close_search() {
    search_open_ = false;
    SDL_StopTextInput(window_);
}

// Matches labels, text, file names and the text inside PDF pages. Best
// first: title starts with the query, then a word does, then anywhere.
void App::run_search() {
    search_results_.clear();
    search_sel_ = 0;
    std::string q = lower(search_query_);
    if (q.empty()) {
        return;
    }
    auto snippet = [](const std::string& text, size_t pos, size_t qlen) {
        size_t start = pos > 30 ? pos - 30 : 0;
        std::string out = text.substr(start, qlen + 60);
        for (char& c : out) {
            if (c == '\n' || c == '\r' || c == '\t') {
                c = ' ';
            }
        }
        return (start > 0 ? "…" : "") + out + (start + qlen + 60 < text.size() ? "…" : "");
    };
    auto score_of = [&](const std::string& hay_lower, size_t pos) {
        if (pos == 0) {
            return 0;
        }
        char before = hay_lower[pos - 1];
        return (before == ' ' || before == '\n') ? 1 : 2;
    };
    std::function<void(Node&, int)> walk = [&](Node& n, int d) {
        if (n.kind != Kind::Root) {
            std::string name = display_name(n);
            std::string hay = lower(n.kind == Kind::PdfPage ? "" : n.text + "\n" + name);
            size_t pos = hay.find(q);
            if (pos != std::string::npos) {
                std::string detail = n.text.find('\n') != std::string::npos &&
                                             pos < n.text.size()
                                         ? snippet(n.text, pos, q.size())
                                         : "";
                search_results_.push_back({&n, name, detail, score_of(hay, pos), d});
            }
            if (n.kind == Kind::PdfPage) {
                const std::string& text = assets_.pdf_page_text(n.asset, n.page);
                size_t p = lower(text).find(q);
                if (p != std::string::npos) {
                    std::string title = (n.parent ? display_name(*n.parent) : "PDF") +
                                        " · page " + std::to_string(n.page + 1);
                    search_results_.push_back({&n, title, snippet(text, p, q.size()),
                                               3, d});
                }
            }
        }
        for (auto& c : n.children) {
            walk(*c, d + 1);
        }
    };
    walk(*root_, 0);
    std::stable_sort(search_results_.begin(), search_results_.end(),
                     [](const SearchResult& a, const SearchResult& b) {
                         return std::tie(a.score, a.depth) < std::tie(b.score, b.depth);
                     });
    if (search_results_.size() > 50) {
        search_results_.resize(50);
    }
}

void App::search_go(int index) {
    if (index < 0 || index >= int(search_results_.size())) {
        return;
    }
    Node* n = search_results_[size_t(index)].node;
    close_search();
    fly_to(n);
    selection_.clear();
    if (n->kind != Kind::PdfPage) {
        selection_ = {n};
    }
}

void App::draw_search() {
    if (!search_open_) {
        return;
    }
    NVGcontext* vg = vg_;
    double w = std::min(560.0, win_w_ - 32.0);
    size_t shown = std::min<size_t>(search_results_.size(), 8);
    bool empty_msg = !search_query_.empty() && search_results_.empty();
    double h = 52 + (shown ? 8 + shown * 52.0 : 0) + (empty_msg ? 44 : 0);
    Rect panel{std::round((win_w_ - w) / 2), 72, w, h};
    search_panel_ = panel;
    ui::island(vg, panel);
    add_hit(panel, nullptr);

    // Input row.
    ui::icon(vg, ui::Icon::Search, {panel.x + 8, panel.y + 6, 40, 40}, ui::rgb(ui::muted));
    double tx = panel.x + 50, ty = panel.y + 26;
    float tw = 0;
    if (search_query_.empty()) {
        ui::label(vg, "Search labels, notes and PDFs…", tx, ty, 15, ui::rgb(0xadb5bd));
    } else {
        tw = ui::label(vg, search_query_, tx, ty, 15, ui::rgb(ui::ink));
    }
    if (std::fmod(now_ - caret_blink_start_, 1.0) < 0.6) {
        nvgBeginPath(vg);
        nvgRect(vg, float(tx + tw + 1), float(ty - 10), 1.5f, 20);
        nvgFillColor(vg, ui::rgb(ui::primary));
        nvgFill(vg);
    }
    ui::label(vg, "Esc", panel.right() - 16, ty, 11.5f, ui::rgb(0xadb5bd), false,
              NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);

    if (shown || empty_msg) {
        nvgBeginPath(vg);
        nvgRect(vg, float(panel.x), float(panel.y + 52), float(panel.w), 1);
        nvgFillColor(vg, ui::rgb(ui::line));
        nvgFill(vg);
    }
    if (empty_msg) {
        ui::label(vg, "No matches", tx, panel.y + 52 + 22, 13, ui::rgb(ui::muted));
    }

    // Keep the selected row in view.
    size_t first = 0;
    if (search_sel_ >= int(shown)) {
        first = size_t(search_sel_) - shown + 1;
    }
    for (size_t i = 0; i < shown; ++i) {
        size_t idx = first + i;
        const SearchResult& res = search_results_[idx];
        Rect row{panel.x + 6, panel.y + 58 + i * 52.0, panel.w - 12, 48};
        bool active = int(idx) == search_sel_;
        ui::button_bg(vg, row, active, row.contains(mouse_));

        std::string path;
        for (Node* p = res.node->parent; p; p = p->parent) {
            path = display_name(*p) + (path.empty() ? "" : " › " + path);
        }
        std::string sub = res.detail.empty() ? path : res.detail;

        nvgSave(vg);
        nvgScissor(vg, float(row.x), float(row.y), float(row.w - 8), float(row.h));
        ui::label(vg, res.title, row.x + 12, row.y + 16, 13.5f, ui::rgb(ui::ink), true);
        ui::label(vg, sub, row.x + 12, row.y + 34, 12, ui::rgb(ui::muted));
        nvgRestore(vg);
        int go = int(idx);
        add_hit(row, [this, go] { search_go(go); });
    }
}

// ---------------------------------------------------------------- input

void App::mouse_down(Vec2 p, int button, int clicks, Mods m) {
    mouse_ = p;
    flying_ = false;
    if (search_open_ && !search_panel_.contains(p)) {
        close_search();
        return;
    }
    if (button == SDL_BUTTON_LEFT && ui_click(p)) {
        return;
    }
    if (button == SDL_BUTTON_MIDDLE || (button == SDL_BUTTON_LEFT &&
                                        (tool_ == Tool::Hand || space_down_))) {
        drag_ = Drag::Pan;
        drag_last_ = p;
        return;
    }
    if (button == SDL_BUTTON_X1) { // mouse "back": zoom out a level
        fly_out();
        return;
    }
    if (button == SDL_BUTTON_X2) { // mouse "forward": dive into the shape
        Node* box = container_at(p, nullptr);
        if (!is_ancestor(box, anchor_)) {
            fly_to(box);
        }
        return;
    }
    if (button != SDL_BUTTON_LEFT) {
        return;
    }
    drag_start_ = drag_last_ = p;
    moved_ = false;

    if (editing_) {
        auto r = screen_rect_of(editing_);
        if (r && r->expanded(4).contains(p)) {
            // Click in the text being edited: put the caret there.
            if (auto t = xform_of(editing_);
                t && (editing_->kind == Kind::Text || editing_->kind == Kind::Code)) {
                caret_ = caret_at(vg_, *editing_, *t, p, raw_line());
                const std::string& s = editing_->text;
                if (vim_.mode != vim::Mode::Insert && caret_ > 0 &&
                    (caret_ == s.size() || s[caret_] == '\n') &&
                    s[caret_ - 1] != '\n') {
                    caret_ = prev_cp(s, caret_); // Normal mode sits on a character
                }
                caret_blink_start_ = now_;
            }
            return;
        }
        finish_text_edit();
        if (tool_ == Tool::Select) {
            return;
        }
    }

    switch (tool_) {
    case Tool::Text: {
        Node* parent = container_at(p, nullptr);
        auto tp = xform_of(parent);
        if (!tp) {
            return;
        }
        auto n = make_node(Kind::Text);
        n->style = style_;
        n->style.font_size = font_px_ / tp->s;
        Vec2 q = tp->to_local(p);
        n->x = q.x;
        n->y = q.y - n->style.font_size * text::line_height / 2;
        fit_text_bounds(vg_, *n);
        Node& added = add_child(*parent, std::move(n));
        tool_ = Tool::Select;
        start_text_edit(added);
        return;
    }
    case Tool::Eraser:
        drag_ = Drag::Erase;
        erasing_.clear();
        if (Node* n = pick(p)) {
            erasing_.insert(n);
        }
        return;
    case Tool::Select:
        break;
    default:
        if (is_shape_tool(tool_)) {
            start_create(p, m);
        }
        return;
    }

    // Double-click: on words, edit them; inside a shape, dive into it; on
    // the empty background, start a new text. A blank new shape gets its
    // label edited first, since it has nothing inside to dive into yet.
    Node* hit = pick(p);
    if (clicks >= 2) {
        pending_link_.clear();
        if (hit && (hit->kind == Kind::Text || hit->kind == Kind::Code)) {
            start_text_edit(*hit);
            return;
        }
        if (hit && hit->is_linear() && hit->kind != Kind::Draw) {
            start_text_edit(*hit); // a label on the arrow or line
            return;
        }
        if (hit && !hit->is_container()) {
            return; // drawings, images
        }
        Node* box = hit ? hit : container_at(p, nullptr);
        if (is_ancestor(box, anchor_)) { // the world we're already in
            tool_ = Tool::Text;
            mouse_down(p, button, 1, m);
        } else if (box->kind != Kind::Pdf &&
                   ((box->text.empty() && box->children.empty()) ||
                    on_label(*box, p))) {
            start_text_edit(*box);
        } else {
            selection_.clear();
            fly_to(box);
        }
        return;
    }

    // A click on a checkbox in a note ticks it.
    if (!editing_ && clicks == 1 && hit && hit->kind == Kind::Text) {
        if (auto t = xform_of(hit)) {
            double em = hit->style.font_size * t->s / hit->w;
            auto L = note::layout(vg_, hit->text, hit->w / hit->style.font_size, -1);
            for (const auto& d : L->decos) {
                Rect box{t->ox + d.x * em, t->oy + d.y * em, d.w * em, d.h * em};
                if (d.kind == note::DecoKind::Checkbox && box.expanded(3).contains(p) &&
                    d.pos + 1 < hit->text.size()) {
                    hit->text[d.pos + 1] = d.checked ? ' ' : 'x';
                    commit();
                    return;
                }
            }
        }
    }

    // A click on a [[link]] follows it, unless a second click turns it into
    // a double-click (see update()).
    if (!editing_) {
        if (auto title = link_at(p)) {
            pending_link_ = *title;
            pending_link_at_ = now_;
        }
    }

    // Select tool: handles first, then shapes, then empty space.
    if (selection_.size() == 1) {
        Node* n = selection_[0];
        auto t = xform_of(n);
        if (t && (n->kind == Kind::Line || n->kind == Kind::Arrow)) {
            auto pts = screen_points(*n, *t);
            for (int i : {0, int(pts.size()) - 1}) {
                if (length(p - pts[i]) <= 8) {
                    drag_ = Drag::Endpoint;
                    resize_handle_ = i;
                    return;
                }
            }
        } else if (t && !is_ancestor(n, anchor_)) {
            Rect r = screen_rect(*n, *t).expanded(4);
            Vec2 hp[8] = {{r.x, r.y}, {r.center().x, r.y}, {r.right(), r.y},
                          {r.right(), r.center().y}, {r.right(), r.bottom()},
                          {r.center().x, r.bottom()}, {r.x, r.bottom()},
                          {r.x, r.center().y}};
            for (int i = 0; i < 8; ++i) {
                if (std::abs(p.x - hp[i].x) <= 6 && std::abs(p.y - hp[i].y) <= 6) {
                    drag_ = Drag::Resize;
                    resize_handle_ = i;
                    resize_orig_screen_ = screen_rect(*n, *t);
                    resize_orig_font_ = n->style.font_size;
                    return;
                }
            }
        }
    }

    if (hit) {
        bool selected = std::ranges::find(selection_, hit) != selection_.end();
        if (m.shift) {
            if (selected) {
                std::erase(selection_, hit);
            } else {
                selection_.push_back(hit);
            }
        } else if (!selected) {
            selection_ = {hit};
        }
        drag_ = Drag::Move;
        return;
    }
    if (!m.shift) {
        selection_.clear();
    }
    drag_ = Drag::Band;
}

void App::mouse_move(Vec2 p, Mods m) {
    Vec2 d = p - drag_last_;
    mouse_ = p;
    switch (drag_) {
    case Drag::Pan:
        cam_.pan(d.x, d.y);
        break;
    case Drag::Create:
        if (creating_) {
            update_create(p, m);
        }
        break;
    case Drag::Move:
        if (length(p - drag_start_) < 3 && !moved_) {
            return; // don't drag on a sloppy click
        }
        for (Node* n : top_level_selection()) {
            if (is_ancestor(n, anchor_)) {
                continue;
            }
            if (auto tp = xform_of(n->parent)) {
                n->x += d.x / tp->s;
                n->y += d.y / tp->s;
                moved_ = true;
            }
        }
        break;
    case Drag::Resize: {
        Node* n = selection_.empty() ? nullptr : selection_[0];
        auto tp = n ? xform_of(n->parent) : std::nullopt;
        if (!tp) {
            break;
        }
        const Rect& o = resize_orig_screen_;
        int h = resize_handle_;
        bool L = h == 0 || h == 6 || h == 7, R = h == 2 || h == 3 || h == 4;
        bool T = h == 0 || h == 1 || h == 2, B = h == 4 || h == 5 || h == 6;
        Vec2 dd = p - drag_start_;
        double l = o.x + (L ? dd.x : 0), r = o.right() + (R ? dd.x : 0);
        double t = o.y + (T ? dd.y : 0), b = o.bottom() + (B ? dd.y : 0);
        bool side = (h == 3 || h == 7) && n->kind == Kind::Text; // note width
        bool keep = !side && (m.shift || n->kind == Kind::Text || n->kind == Kind::Image ||
                              n->kind == Kind::Pdf || n->kind == Kind::Code);
        if (keep) {
            double sx = (r - l) / o.w, sy = (b - t) / o.h;
            double k = (L || R) && (T || B) ? std::max(sx, sy) : (L || R) ? sx : sy;
            k = std::max(k, 4 / std::min(o.w, o.h));
            double nw = o.w * k, nh = o.h * k;
            l = L ? o.right() - nw : R ? o.x : o.center().x - nw / 2;
            t = T ? o.bottom() - nh : B ? o.y : o.center().y - nh / 2;
            r = l + nw;
            b = t + nh;
        }
        Rect nr = Rect::from_points({l, t}, {r, b});
        nr.w = std::max(nr.w, 2.0);
        nr.h = std::max(nr.h, 2.0);
        n->x = (nr.x - tp->ox) / tp->s;
        n->y = (nr.y - tp->oy) / tp->s;
        n->w = nr.w / tp->s;
        n->h = nr.h / tp->s;
        if ((n->kind == Kind::Text && !side) || n->kind == Kind::Code) {
            n->style.font_size = resize_orig_font_ * nr.h / o.h;
        }
        if (n->kind == Kind::Text) {
            n->w = std::max(n->w, 4 * n->style.font_size);
            fit_text_bounds(vg_, *n, n == editing_ ? raw_line() : -1);
        }
        moved_ = true;
        break;
    }
    case Drag::Endpoint: {
        Node* n = selection_.empty() ? nullptr : selection_[0];
        auto tp = n ? xform_of(n->parent) : std::nullopt;
        if (!tp) {
            break;
        }
        auto pts = linear_local_points(*n);
        pts[size_t(resize_handle_)] = tp->to_local(p);
        set_linear_points(*n, pts);
        moved_ = true;
        break;
    }
    case Drag::Band: {
        Rect band = Rect::from_points(drag_start_, p);
        if (!m.shift) {
            selection_.clear();
        }
        std::function<void(Node&, Xform)> collect = [&](Node& n, Xform t) {
            for (auto& c : n.children) {
                Xform ct = child_xform(t, *c);
                Rect r = screen_rect(*c, ct);
                if (band.contains(r)) {
                    if (std::ranges::find(selection_, c.get()) == selection_.end()) {
                        selection_.push_back(c.get());
                    }
                } else if (c->is_container() && band.intersects(r)) {
                    collect(*c, ct);
                }
            }
        };
        collect(*anchor_, cam_.xform());
        break;
    }
    case Drag::Erase:
        if (Node* n = pick(p)) {
            erasing_.insert(n);
        }
        break;
    case Drag::None:
        break;
    }
    drag_last_ = p;
}

void App::mouse_up(Vec2 p, int button, Mods) {
    mouse_ = p;
    Drag d = drag_;
    drag_ = Drag::None;
    if (button == SDL_BUTTON_MIDDLE || d == Drag::Pan) {
        return;
    }
    switch (d) {
    case Drag::Create:
        if (creating_) {
            finish_create();
        }
        break;
    case Drag::Move:
        if (moved_) {
            // An arrow moved on its own lets go of its shapes (like Excalidraw).
            for (Node* n : selection_) {
                for (std::string* b : {&n->start_bind, &n->end_bind}) {
                    bool target_moved = std::ranges::any_of(
                        selection_, [&](Node* s) { return s->id == *b; });
                    if (!b->empty() && !target_moved) {
                        b->clear();
                    }
                }
            }
            // Dropped inside another shape? It now lives there.
            for (Node* n : top_level_selection()) {
                auto r = screen_rect_of(n);
                if (!r || is_ancestor(n, anchor_)) {
                    continue;
                }
                Node* np = container_at(r->center(), n);
                if (np && np != n->parent && !is_ancestor(n, np)) {
                    reparent(*n, *np);
                }
            }
            commit();
        }
        break;
    case Drag::Resize:
        if (moved_) {
            commit();
        }
        break;
    case Drag::Endpoint:
        if (moved_ && !selection_.empty()) {
            bind_endpoints(*selection_[0]);
            commit();
        }
        break;
    case Drag::Erase: {
        bool any = false;
        for (Node* n : erasing_) {
            bool nested = false;
            for (Node* o : erasing_) {
                if (o != n && is_ancestor(o, n)) {
                    nested = true;
                }
            }
            if (!nested && n->parent && !is_ancestor(n, anchor_)) {
                std::erase(selection_, n);
                detach(*n);
                any = true;
            }
        }
        erasing_.clear();
        if (any) {
            commit();
        }
        break;
    }
    default:
        break;
    }
}

void App::wheel(Vec2 p, double dx, double dy, Mods m) {
    mouse_ = p;
    flying_ = false;
    if (m.shift && !m.ctrl) {
        cam_.pan(-dy * 40, 0);
        return;
    }
    if (dx != 0) {
        cam_.pan(-dx * 40, 0);
    }
    if (dy != 0) {
        cam_.zoom_at(p, std::pow(1.15, dy));
    }
}

void App::pinch(double factor) {
    flying_ = false;
    cam_.zoom_at(mouse_, factor);
}

void App::key_up(SDL_Keycode key) {
    if (key == SDLK_SPACE) {
        space_down_ = false;
    }
}

void App::key_down(SDL_Keycode key, Mods m) {
    if (search_open_) {
        caret_blink_start_ = now_;
        switch (key) {
        case SDLK_ESCAPE:
            close_search();
            return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            search_go(search_sel_);
            return;
        case SDLK_DOWN:
            search_sel_ = std::min(search_sel_ + 1, int(search_results_.size()) - 1);
            search_sel_ = std::max(search_sel_, 0);
            return;
        case SDLK_UP:
            search_sel_ = std::max(search_sel_ - 1, 0);
            return;
        case SDLK_BACKSPACE:
            search_query_.erase(prev_cp(search_query_, search_query_.size()));
            run_search();
            return;
        case SDLK_V:
            if (m.ctrl) {
                char* raw = SDL_GetClipboardText();
                search_query_ += raw ? raw : "";
                SDL_free(raw);
                run_search();
            }
            return;
        case SDLK_F:
            if (m.ctrl) {
                close_search();
            }
            return;
        default:
            return;
        }
    }
    if (editing_) {
        std::string& s = editing_->text;
        caret_blink_start_ = now_;
        auto refit = [&] { refit_editing(); };
        if (vim_.mode != vim::Mode::Insert) {
            // Normal and Visual mode: typed characters arrive in text_input.
            auto feed = [&](const char* k) {
                if (vim_.key(s, caret_, k)) {
                    refit();
                }
            };
            switch (key) {
            case SDLK_ESCAPE:
                if (vim_.mode == vim::Mode::Normal && vim_.pending().empty()) {
                    finish_text_edit();
                } else {
                    vim_.mode = vim::Mode::Normal;
                    vim_.cancel();
                }
                return;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (m.ctrl) {
                    finish_text_edit();
                } else {
                    feed("j");
                }
                return;
            case SDLK_R:
                if (m.ctrl && vim_.redo(s, caret_)) {
                    refit();
                }
                return;
            case SDLK_LEFT: case SDLK_BACKSPACE: feed("h"); return;
            case SDLK_RIGHT: feed("l"); return;
            case SDLK_UP: feed("k"); return;
            case SDLK_DOWN: feed("j"); return;
            default:
                return;
            }
        }
        if (auto q = link_query(); q && !link_matches(q->second).empty()) {
            switch (key) {
            case SDLK_UP: --link_sel_; return;
            case SDLK_DOWN: ++link_sel_; return;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_TAB:
                accept_link();
                return;
            default:
                break;
            }
        }
        switch (key) {
        case SDLK_ESCAPE:
            vim_.to_normal(s, caret_);
            return;
        case SDLK_TAB:
            if (editing_->kind == Kind::Code) {
                s.insert(caret_, "    ");
                caret_ += 4;
                refit();
            } else if (editing_->kind == Kind::Text) {
                // Indent or outdent a list item.
                size_t ls = line_start(s, caret_);
                if (note::list_item(s, ls).marker_len) {
                    if (!m.shift) {
                        s.insert(ls, "  ");
                        caret_ += 2;
                    } else {
                        size_t n = std::min<size_t>(2, s.find_first_not_of(' ', ls) - ls);
                        s.erase(ls, n);
                        caret_ -= std::min(caret_ - ls, n);
                    }
                    refit();
                }
            }
            return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: {
            if (m.ctrl) {
                finish_text_edit();
                return;
            }
            size_t ls = line_start(s, caret_);
            size_t le = s.find('\n', caret_);
            std::string line = s.substr(ls, caret_ - ls);
            size_t a = line.find_first_not_of(" \t");
            std::string trimmed = a == std::string::npos ? "" : line.substr(a);
            while (!trimmed.empty() && trimmed.back() == ' ') {
                trimmed.pop_back();
            }
            bool at_eol = le == caret_ || (le == std::string::npos && caret_ == s.size());
            if (at_eol && editing_->kind != Kind::Code && !editing_->is_linear() &&
                trimmed.starts_with("```")) {
                open_code_block(trimmed.substr(3));
                return;
            }
            if (at_eol && editing_->kind == Kind::Code && trimmed == "```") {
                close_code_block();
                return;
            }
            // Lists continue on Enter; Enter on an empty item ends the list.
            if (editing_->kind == Kind::Text) {
                note::ListItem it = note::list_item(s, ls);
                size_t le2 = std::min(s.find('\n', ls), s.size());
                if (it.marker_len && le2 == ls + it.marker_len && caret_ == le2) {
                    s.erase(ls, it.marker_len);
                    caret_ = ls;
                    refit();
                    return;
                }
                if (it.marker_len && caret_ >= ls + it.marker_len) {
                    s.insert(caret_, "\n" + it.next);
                    caret_ += 1 + it.next.size();
                    refit();
                    return;
                }
            }
            // Code keeps the indentation of the line above.
            std::string indent = editing_->kind == Kind::Code
                                     ? line.substr(0, a == std::string::npos ? line.size() : a)
                                     : "";
            s.insert(caret_, "\n" + indent);
            caret_ += 1 + indent.size();
            refit();
            return;
        }
        case SDLK_BACKSPACE: {
            // At the start of a list item, Backspace turns it back into text.
            if (editing_->kind == Kind::Text) {
                size_t ls = line_start(s, caret_);
                note::ListItem it = note::list_item(s, ls);
                if (it.marker_len && caret_ == ls + it.marker_len) {
                    s.erase(ls, it.marker_len);
                    caret_ = ls;
                    refit();
                    return;
                }
            }
            size_t p = prev_cp(s, caret_);
            s.erase(p, caret_ - p);
            caret_ = p;
            refit();
            return;
        }
        case SDLK_DELETE:
            s.erase(caret_, next_cp(s, caret_) - caret_);
            refit();
            return;
        case SDLK_LEFT:
            caret_ = prev_cp(s, caret_);
            return;
        case SDLK_RIGHT:
            caret_ = next_cp(s, caret_);
            return;
        case SDLK_HOME:
            caret_ = line_start(s, caret_);
            return;
        case SDLK_END: {
            size_t nl = s.find('\n', caret_);
            caret_ = nl == std::string::npos ? s.size() : nl;
            return;
        }
        case SDLK_UP:
        case SDLK_DOWN: {
            size_t ls = line_start(s, caret_);
            size_t col = caret_ - ls;
            if (key == SDLK_UP) {
                if (ls == 0) {
                    caret_ = 0;
                    return;
                }
                size_t prev_end = ls - 1;
                caret_ = std::min(line_start(s, prev_end) + col, prev_end);
            } else {
                size_t next_start = s.find('\n', caret_);
                if (next_start == std::string::npos) {
                    caret_ = s.size();
                    return;
                }
                ++next_start;
                size_t next_end = s.find('\n', next_start);
                next_end = next_end == std::string::npos ? s.size() : next_end;
                caret_ = std::min(next_start + col, next_end);
            }
            while (caret_ > 0 && caret_ < s.size() && (s[caret_] & 0xc0) == 0x80) {
                --caret_;
            }
            return;
        }
        case SDLK_V:
            if (m.ctrl) {
                char* raw = SDL_GetClipboardText();
                std::string txt = raw ? raw : "";
                SDL_free(raw);
                s.insert(caret_, txt);
                caret_ += txt.size();
                refit();
            }
            return;
        default:
            return;
        }
    }

    if (m.ctrl) {
        switch (key) {
        case SDLK_F:
            open_search();
            return;
        case SDLK_Z:
            m.shift ? redo() : undo();
            return;
        case SDLK_Y:
        case SDLK_R:
            redo();
            return;
        case SDLK_A:
            select_all();
            return;
        case SDLK_D:
            duplicate_selection();
            return;
        case SDLK_C:
            copy_selection();
            return;
        case SDLK_X:
            copy_selection();
            delete_selection();
            return;
        case SDLK_V:
            paste(mouse_);
            return;
        case SDLK_S: {
            flush();
            toast(store::git_commit(dir_));
            return;
        }
        case SDLK_RIGHTBRACKET:
            move_layer(true);
            return;
        case SDLK_LEFTBRACKET:
            move_layer(false);
            return;
        default:
            return;
        }
    }

    switch (key) {
    case SDLK_SPACE:
        space_down_ = true;
        return;
    case SDLK_ESCAPE:
        if (drag_ == Drag::Create && creating_) {
            detach(*creating_);
            creating_ = nullptr;
            drag_ = Drag::None;
        } else if (tool_ != Tool::Select) {
            tool_ = Tool::Select;
        } else if (!selection_.empty() &&
                   !(selection_.size() == 1 && selection_[0]->id == came_from_id_)) {
            selection_.clear();
        } else {
            fly_out(); // repeated Esc keeps going out, level by level
        }
        return;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (selection_.size() == 1) {
            Node* n = selection_[0];
            if (n->is_container()) {
                fly_to(n); // dive in
            } else {
                edit_selected();
            }
        }
        return;
    case SDLK_BACKSPACE:
        if (selection_.empty() ||
            (selection_.size() == 1 && selection_[0]->id == came_from_id_)) {
            fly_out();
            return;
        }
        [[fallthrough]];
    case SDLK_DELETE:
        delete_selection();
        return;
    case SDLK_LEFT: nudge(m.shift ? -10 : -1, 0); return;
    case SDLK_RIGHT: nudge(m.shift ? 10 : 1, 0); return;
    case SDLK_UP: nudge(0, m.shift ? -10 : -1); return;
    case SDLK_DOWN: nudge(0, m.shift ? 10 : 1); return;
    case SDLK_HOME:
        fly_to_content();
        return;
    case SDLK_SLASH:
        open_search();
        return;
    case SDLK_H: set_tool(Tool::Hand); return;
    case SDLK_V: set_tool(Tool::Select); return;
    case SDLK_R: set_tool(Tool::Rect); return;
    case SDLK_D: set_tool(Tool::Diamond); return;
    case SDLK_O: set_tool(Tool::Ellipse); return;
    case SDLK_A: set_tool(Tool::Arrow); return;
    case SDLK_L: set_tool(Tool::Line); return;
    case SDLK_P: set_tool(Tool::Draw); return;
    case SDLK_T: set_tool(Tool::Text); return;
    case SDLK_E: set_tool(Tool::Eraser); return;
    case SDLK_1:
        if (m.shift) {
            fly_to_content();
        } else {
            set_tool(Tool::Select);
        }
        return;
    case SDLK_2: set_tool(Tool::Rect); return;
    case SDLK_3: set_tool(Tool::Diamond); return;
    case SDLK_4: set_tool(Tool::Ellipse); return;
    case SDLK_5: set_tool(Tool::Arrow); return;
    case SDLK_6: set_tool(Tool::Line); return;
    case SDLK_7: set_tool(Tool::Draw); return;
    case SDLK_8: set_tool(Tool::Text); return;
    case SDLK_9: set_tool(Tool::Image); return;
    case SDLK_0: set_tool(Tool::Eraser); return;
    default:
        return;
    }
}

void App::text_input(const char* utf8) {
    if (search_open_) {
        search_query_ += utf8;
        caret_blink_start_ = now_;
        run_search();
        return;
    }
    if (!editing_) {
        return;
    }
    caret_blink_start_ = now_;
    std::string_view in = utf8;
    // Outside Insert mode each character is a vim command; if one of them
    // enters Insert mode, the rest is typed text.
    while (!in.empty() && vim_.mode != vim::Mode::Insert) {
        size_t j = 1;
        while (j < in.size() && (in[j] & 0xc0) == 0x80) {
            ++j;
        }
        vim_.key(editing_->text, caret_, in.substr(0, j));
        in.remove_prefix(j);
    }
    editing_->text.insert(caret_, in);
    caret_ += in.size();
    refit_editing();
}

// ---------------------------------------------------------------- frame

void App::update(double now, int win_w, int win_h) {
    double dt = last_frame_ > 0 ? std::min(now - last_frame_, 0.1) : 1 / 60.0;
    last_frame_ = now_ = now;
    frame_dt_ = dt;
    win_w_ = win_w;
    win_h_ = win_h;

    std::vector<std::string> files;
    {
        std::lock_guard lock(pending_mutex_);
        files.swap(pending_files_);
    }
    for (size_t i = 0; i < files.size(); ++i) {
        insert_file(files[i], {win_w_ / 2.0 + 24.0 * i, win_h_ / 2.0 + 24.0 * i});
    }

    if (!pending_link_.empty() && (moved_ || drag_ != Drag::None)) {
        if (moved_) {
            pending_link_.clear(); // it was a drag
        }
    } else if (!pending_link_.empty() && now_ - pending_link_at_ > 0.3) {
        std::string title = std::move(pending_link_);
        pending_link_.clear();
        follow_link(title);
    }
    refit_editing();
    step_fly(dt);
    clamp_zoom();
    reroot();
    update_bindings();

    // Selected nodes far above the visible levels can't be edited anymore.
    std::erase_if(selection_, [&](Node* n) { return !xform_of(n); });

    if (dirty_ && now_ - last_change_ > 0.8 && drag_ == Drag::None) {
        flush();
    }
}

void App::render(float pixel_ratio) {
    pixel_ratio_ = pixel_ratio;
    nvgBeginFrame(vg_, float(win_w_), float(win_h_), pixel_ratio);

    RenderCtx ctx{vg_, assets_, {0, 0, double(win_w_), double(win_h_)}, pixel_ratio, {}};
    ctx.dt = frame_dt_;
    ctx.editing = editing_;
    ctx.caret = caret_;
    for (Node* n : erasing_) {
        ctx.faded.insert(n);
    }
    auto [n, t] = draw_root();
    draw_subtree(ctx, *n, t);
    labels_moving_ = ctx.animating;

    draw_overlay();
    draw_ui();
    draw_link_popup();
    nvgEndFrame(vg_);

    hits_ = std::move(next_hits_);
    next_hits_.clear();
    assets_.end_frame();
}

// ---------------------------------------------------------------- overlay

void App::draw_overlay() {
    NVGcontext* vg = vg_;
    NVGcolor accent = ui::rgb(ui::primary);

    // Hover hint.
    bool over_ui = std::ranges::any_of(hits_, [&](const Hit& h) { return h.r.contains(mouse_); });
    if (tool_ == Tool::Select && drag_ == Drag::None && !editing_ && !over_ui) {
        Node* h = pick(mouse_);
        if (h && std::ranges::find(selection_, h) == selection_.end()) {
            if (auto r = screen_rect_of(h); r && r->w < win_w_ && r->h < win_h_) {
                nvgBeginPath(vg);
                nvgRect(vg, float(r->x - 4), float(r->y - 4), float(r->w + 8),
                        float(r->h + 8));
                nvgStrokeColor(vg, ui::rgb(ui::primary, 90));
                nvgStrokeWidth(vg, 1);
                nvgStroke(vg);
            }
        }
    }

    for (Node* n : selection_) {
        auto t = xform_of(n);
        if (!t) {
            continue;
        }
        Rect r = screen_rect(*n, *t).expanded(4);
        bool linear2 = n->kind == Kind::Line || n->kind == Kind::Arrow;
        if (!(selection_.size() == 1 && linear2)) {
            nvgBeginPath(vg);
            nvgRect(vg, float(r.x), float(r.y), float(r.w), float(r.h));
            nvgStrokeColor(vg, accent);
            nvgStrokeWidth(vg, 1);
            nvgStroke(vg);
        }
        if (selection_.size() != 1 || editing_ || is_ancestor(n, anchor_)) {
            continue;
        }
        if (linear2) {
            for (Vec2 p : screen_points(*n, *t)) {
                nvgBeginPath(vg);
                nvgCircle(vg, float(p.x), float(p.y), 5);
                nvgFillColor(vg, nvgRGB(255, 255, 255));
                nvgFill(vg);
                nvgStrokeColor(vg, accent);
                nvgStroke(vg);
            }
            continue;
        }
        Vec2 hp[8] = {{r.x, r.y}, {r.center().x, r.y}, {r.right(), r.y},
                      {r.right(), r.center().y}, {r.right(), r.bottom()},
                      {r.center().x, r.bottom()}, {r.x, r.bottom()},
                      {r.x, r.center().y}};
        for (Vec2 p : hp) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, float(p.x - 4), float(p.y - 4), 8, 8, 2);
            nvgFillColor(vg, nvgRGB(255, 255, 255));
            nvgFill(vg);
            nvgStrokeColor(vg, accent);
            nvgStrokeWidth(vg, 1);
            nvgStroke(vg);
        }
    }

    Node* linear = drag_ == Drag::Create ? creating_
                   : drag_ == Drag::Endpoint && !selection_.empty() ? selection_[0]
                                                                    : nullptr;
    if (linear && (linear->kind == Kind::Arrow || linear->kind == Kind::Line)) {
        if (Node* tgt = bind_target_at(mouse_, linear)) {
            if (auto r = screen_rect_of(tgt)) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, float(r->x - 6), float(r->y - 6), float(r->w + 12),
                               float(r->h + 12), 6);
                nvgStrokeColor(vg, ui::rgb(ui::primary, 140));
                nvgStrokeWidth(vg, 3);
                nvgStroke(vg);
            }
        }
    }

    if (drag_ == Drag::Band) {
        Rect b = Rect::from_points(drag_start_, mouse_);
        nvgBeginPath(vg);
        nvgRect(vg, float(b.x), float(b.y), float(b.w), float(b.h));
        nvgFillColor(vg, ui::rgb(ui::primary, 20));
        nvgFill(vg);
        nvgStrokeColor(vg, accent);
        nvgStrokeWidth(vg, 1);
        nvgStroke(vg);
    }

    // Text caret: a bar in Insert mode, a block in Normal mode, and the
    // Visual selection.
    if (editing_) {
        if (auto t = xform_of(editing_)) {
            const std::string& s = editing_->text;
            int raw = raw_line();
            auto box = [&](size_t pos) { return caret_box(vg, *editing_, *t, pos, raw); };
            if (vim_.mode == vim::Mode::Visual || vim_.mode == vim::Mode::VisualLine) {
                auto [a, b] = vim_.selection(s, caret_);
                for (const auto& span : screen_lines(vg, *editing_, *t, raw)) {
                    size_t sa = std::max(a, span.begin), sb = std::min(b, span.end);
                    bool newline = b > span.end && a <= span.end && span.end < s.size();
                    if (sa >= sb && !newline) {
                        continue;
                    }
                    CaretBox A = box(sa), B = box(std::max(sa, sb));
                    double w = B.x - A.x + (newline ? A.h * 0.3 : 0);
                    nvgBeginPath(vg);
                    nvgRect(vg, float(A.x), float(A.y), float(std::max(w, 1.0)), float(A.h));
                    nvgFillColor(vg, ui::rgb(ui::primary, 70));
                    nvgFill(vg);
                }
            }
            CaretBox c = box(caret_);
            if (vim_.mode == vim::Mode::Insert) {
                if (std::fmod(now_ - caret_blink_start_, 1.0) < 0.6) {
                    nvgBeginPath(vg);
                    nvgRect(vg, float(c.x), float(c.y + c.h * 0.08),
                            float(std::max(1.0, c.h / 20)), float(c.h * 0.84));
                    nvgFillColor(vg, accent);
                    nvgFill(vg);
                }
            } else {
                size_t nx = caret_ < s.size() && s[caret_] != '\n' ? next_cp(s, caret_) : caret_;
                double w = nx > caret_ ? box(nx).x - c.x : 0;
                if (w <= 0) {
                    w = c.h * 0.45;
                }
                nvgBeginPath(vg);
                nvgRect(vg, float(c.x), float(c.y + c.h * 0.08), float(w), float(c.h * 0.84));
                nvgFillColor(vg, ui::rgb(ui::primary, 110));
                nvgFill(vg);
            }
        }
    }

    // Empty map welcome.
    if (anchor_ == root_.get() && root_->children.empty() && !editing_) {
        text::draw_line(vg, text::Hand, "Draw something, then scroll to zoom into it",
                        win_w_ / 2.0 - text::measure(vg, text::Hand,
                                                     "Draw something, then scroll to zoom into it", 24) / 2,
                        win_h_ / 2.0 - 30, 24, ui::rgb(0xadb5bd),
                        {0, 0, double(win_w_), double(win_h_)});
    }
}

// ---------------------------------------------------------------- ui

void App::add_hit(Rect r, std::function<void()> action) {
    next_hits_.push_back({r, std::move(action)});
}

bool App::ui_click(Vec2 p) {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (it->r.contains(p)) {
            if (it->action) {
                it->action();
            }
            return true;
        }
    }
    return false;
}

bool App::button(Rect r, ui::Icon icon, bool active, std::function<void()> action,
                 const char* hint) {
    bool hovered = r.contains(mouse_) && drag_ == Drag::None;
    ui::button_bg(vg_, r, active, hovered);
    ui::icon(vg_, icon, r, ui::rgb(active ? 0x030064 : ui::ink));
    if (hint) {
        ui::label(vg_, hint, r.right() - 3, r.bottom() - 2, 9, ui::rgb(ui::muted),
                  false, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
    }
    add_hit(r, std::move(action));
    return hovered;
}

bool App::style_panel_visible() const {
    if (!selection_.empty()) {
        return true;
    }
    return is_shape_tool(tool_) || tool_ == Tool::Text;
}

void App::draw_style_panel() {
    NVGcontext* vg = vg_;
    if (!selection_.empty() && selection_[0]->kind == Kind::Code) {
        const auto& langs = code::languages();
        int cols = 3;
        int rows = int(langs.size() + cols - 1) / cols;
        double x = 16, y = 72, w = 204, h = 12 + 18 + rows * 30 + 8;
        ui::island(vg, {x, y, w, h});
        add_hit({x, y, w, h}, nullptr);
        ui::label(vg, "Language", x + 12, y + 18, 11.5f, ui::rgb(0x495057));
        for (size_t i = 0; i < langs.size(); ++i) {
            Rect r{x + 12 + double(i % cols) * 61, y + 30 + double(i / cols) * 30, 57, 26};
            std::string name = langs[i].name;
            bool active = selection_[0]->lang == name ||
                          (selection_[0]->lang.empty() && name == "text");
            ui::button_bg(vg, r, active, r.contains(mouse_));
            ui::label(vg, name == "javascript" ? "js" : name == "typescript" ? "ts" : name,
                      r.center().x, r.center().y, 12, ui::rgb(active ? ui::primary : ui::ink),
                      false, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            add_hit(r, [this, name] {
                code_lang_ = name;
                for (Node* n : selection_) {
                    if (n->kind == Kind::Code) {
                        n->lang = name;
                    }
                }
                commit();
            });
        }
        return;
    }
    // Values shown: the first selected node, or the style for new shapes.
    Node* first = selection_.empty() ? nullptr : selection_[0];
    uint32_t stroke = first ? first->style.stroke : style_.stroke;
    uint32_t fill = first ? first->style.fill : style_.fill;
    FillStyle fill_style = first ? first->style.fill_style : style_.fill_style;
    int roughness = first ? first->style.roughness : style_.roughness;
    double sw_px = stroke_px_, font_px = font_px_;
    if (first) {
        if (auto tp = xform_of(first->parent)) {
            sw_px = first->style.stroke_width * tp->s;
            font_px = first->style.font_size * tp->s;
        }
    }
    bool has_text = tool_ == Tool::Text;
    bool has_shape = is_shape_tool(tool_) && tool_ != Tool::Draw &&
                     tool_ != Tool::Line && tool_ != Tool::Arrow;
    for (Node* n : selection_) {
        has_text |= n->kind == Kind::Text || (!n->text.empty() && n->kind != Kind::Pdf);
        has_shape |= n->kind == Kind::Rect || n->kind == Kind::Ellipse ||
                     n->kind == Kind::Diamond;
    }

    // Sections are 58 px tall, except the color grids.
    constexpr double stroke_h = 18 + 28 + 2 * 17 + 12, bg_h = 18 + 28 + 3 * 17 + 12;
    double x = 16, y = 72, w = 236;
    double rows = 2 + (has_shape ? 1 : 0) + (has_text ? 1 : 0) + (first ? 1 : 0);
    double h = 16 + rows * 58 + stroke_h + (has_shape ? bg_h : 0);
    ui::island(vg, {x, y, w, h});
    add_hit({x, y, w, h}, nullptr);

    double cx = x + 12, cy = y + 12;
    auto section = [&](const char* title) {
        ui::label(vg, title, cx, cy + 6, 11.5f, ui::rgb(0x495057));
        cy += 18;
    };
    auto set_style = [this](std::function<void(Style&)> f) {
        f(style_);
        apply_to_selection([&](Node& n) { f(n.style); });
    };

    // Picking a color with nothing selected means "use this, not auto".
    auto pick = [this, set_style](bool is_fill, uint32_t c) {
        if (selection_.empty()) {
            auto_color_ = false;
        }
        set_style([=](Style& s) { (is_fill ? s.fill : s.stroke) = c; });
    };
    // One row of fixed swatches, then a hue x shade grid.
    auto palette = [&](bool is_fill, uint32_t current, std::initializer_list<uint32_t> fixed,
                       std::initializer_list<int> shades) {
        double fx = cx;
        if (is_fill) { // the Auto chip
            Rect r{fx, cy, 46, 22};
            bool on = auto_color_;
            ui::button_bg(vg, r, on, r.contains(mouse_));
            ui::label(vg, "Auto", r.center().x, r.center().y, 12,
                      ui::rgb(on ? ui::primary : ui::ink), true,
                      NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            add_hit(r, [this] {
                auto_color_ = true;
                // Fresh colors for the selected shapes, one at a time.
                for (Node* n : selection_) {
                    if (n->kind == Kind::Rect || n->kind == Kind::Ellipse ||
                        n->kind == Kind::Diamond) {
                        n->style.fill = 0;
                        auto_color(*n);
                    }
                }
                commit();
            });
            fx += 54;
        }
        for (uint32_t c : fixed) {
            Rect r{fx, cy, 22, 22};
            ui::swatch(vg, r, c, current == c);
            add_hit(r, [=] { pick(is_fill, c); });
            fx += 28;
        }
        cy += 28;
        for (int k : shades) {
            for (int hh = 0; hh < ui::n_hues; ++hh) {
                Rect r{cx + hh * 17, cy, 15, 15};
                uint32_t c = ui::shade(hh, k);
                ui::swatch(vg, r, c, current == c);
                add_hit(r, [=] { pick(is_fill, c); });
            }
            cy += 17;
        }
        cy += 12;
    };

    section("Stroke");
    palette(false, stroke, {0x1e1e1eff, 0x868e96ff, 0xffffffff}, {3, 4});

    if (has_shape) {
        section("Background");
        palette(true, fill, {0x00000000, 0xffffffff, 0xf1f3f5ff}, {0, 1, 2});
        section("Fill");
        ui::Icon fills[3] = {ui::Icon::Hachure, ui::Icon::CrossHatch, ui::Icon::Solid};
        for (int i = 0; i < 3; ++i) {
            FillStyle fs = FillStyle(i);
            button({cx + i * 36, cy, 32, 32}, fills[i], fill_style == fs,
                   [=] { set_style([fs](Style& s) { s.fill_style = fs; }); });
        }
        cy += 40;
    }

    section("Stroke width");
    ui::Icon widths[3] = {ui::Icon::Thin, ui::Icon::Bold, ui::Icon::ExtraBold};
    double px_values[3] = {1, 2, 4};
    for (int i = 0; i < 3; ++i) {
        double px = px_values[i];
        button({cx + i * 36, cy, 32, 32}, widths[i], std::abs(sw_px - px) < 0.25,
               [this, px] {
                   stroke_px_ = px;
                   apply_to_selection([&](Node& n) {
                       if (auto tp = xform_of(n.parent)) {
                           n.style.stroke_width = px / tp->s;
                       }
                   });
               });
    }
    cy += 40;

    section("Sloppiness");
    ui::Icon rough_icons[3] = {ui::Icon::Architect, ui::Icon::Artist, ui::Icon::Cartoonist};
    for (int i = 0; i < 3; ++i) {
        button({cx + i * 36, cy, 32, 32}, rough_icons[i], roughness == i,
               [=] { set_style([i](Style& s) { s.roughness = i; }); });
    }
    cy += 40;

    if (has_text) {
        section("Font size");
        const char* names[4] = {"S", "M", "L", "XL"};
        double sizes[4] = {16, 20, 28, 36};
        for (int i = 0; i < 4; ++i) {
            Rect r{cx + i * 36, cy, 32, 32};
            bool active = std::abs(font_px - sizes[i]) < 0.5;
            ui::button_bg(vg, r, active, r.contains(mouse_));
            ui::label(vg, names[i], r.center().x, r.center().y, 13,
                      ui::rgb(ui::ink), true, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            double px = sizes[i];
            add_hit(r, [this, px] {
                font_px_ = px;
                apply_to_selection([&](Node& n) {
                    if (auto tp = xform_of(n.parent)) {
                        n.style.font_size = px / tp->s;
                        if (n.kind == Kind::Text) {
                            fit_text_bounds(vg_, n);
                        }
                    }
                });
            });
        }
        cy += 40;
    }

    if (first) {
        section("Actions");
        button({cx, cy, 32, 32}, ui::Icon::ToBack, false, [this] { move_layer(false); });
        button({cx + 36, cy, 32, 32}, ui::Icon::ToFront, false, [this] { move_layer(true); });
        button({cx + 72, cy, 32, 32}, ui::Icon::Duplicate, false,
               [this] { duplicate_selection(); });
        button({cx + 108, cy, 32, 32}, ui::Icon::Trash, false, [this] { delete_selection(); });
    }
}

void App::draw_ui() {
    NVGcontext* vg = vg_;

    // Toolbar, top center.
    struct ToolDef {
        Tool tool;
        ui::Icon icon;
        const char* key;
    };
    static const ToolDef tools[] = {
        {Tool::Hand, ui::Icon::Hand, "H"},       {Tool::Select, ui::Icon::Select, "1"},
        {Tool::Rect, ui::Icon::Rect, "2"},       {Tool::Diamond, ui::Icon::Diamond, "3"},
        {Tool::Ellipse, ui::Icon::Ellipse, "4"}, {Tool::Arrow, ui::Icon::Arrow, "5"},
        {Tool::Line, ui::Icon::Line, "6"},       {Tool::Draw, ui::Icon::Draw, "7"},
        {Tool::Text, ui::Icon::Text, "8"},       {Tool::Image, ui::Icon::Image, "9"},
        {Tool::Eraser, ui::Icon::Eraser, "0"},
    };
    int n_tools = int(std::size(tools));
    double bw = 36, gap = 2, pad = 4;
    double tw = n_tools * bw + (n_tools - 1) * gap + 2 * pad;
    Rect bar{std::round((win_w_ - tw) / 2), 16, tw, bw + 2 * pad};
    ui::island(vg, bar);
    add_hit(bar, nullptr);
    for (int i = 0; i < n_tools; ++i) {
        Rect r{bar.x + pad + i * (bw + gap), bar.y + pad, bw, bw};
        Tool t = tools[i].tool;
        button(r, tools[i].icon, tool_ == t, [this, t] { set_tool(t); }, tools[i].key);
    }

    // Breadcrumb, top left: where you are in the zoom hierarchy.
    std::vector<Node*> path;
    for (Node* p = anchor_; p; p = p->parent) {
        path.insert(path.begin(), p);
    }
    if (path.size() > 4) {
        path.erase(path.begin() + 1, path.end() - 3);
        path.insert(path.begin() + 1, nullptr); // "…"
    }
    {
        double x = 28, cy = 16 + 22;
        double total = 24;
        std::vector<double> widths;
        nvgFontFace(vg, "ui-medium");
        nvgFontSize(vg, 13);
        for (Node* p : path) {
            std::string name = p ? display_name(*p) : "…";
            float b[4];
            double wdt = nvgTextBounds(vg, 0, 0, name.c_str(), nullptr, b);
            widths.push_back(wdt);
            total += wdt + 22;
        }
        total -= 22;
        Rect island{16, 16, std::min(total, bar.x - 32), 44};
        if (island.w > 60) {
            ui::island(vg, island);
            add_hit(island, nullptr);
            nvgSave(vg);
            nvgScissor(vg, float(island.x), float(island.y), float(island.w - 8),
                       float(island.h));
            for (size_t i = 0; i < path.size(); ++i) {
                Node* p = path[i];
                bool last = i + 1 == path.size();
                std::string name = p ? display_name(*p) : "…";
                Rect hit{x - 4, island.y + 8, widths[i] + 8, 28};
                if (p && !last && hit.contains(mouse_)) {
                    ui::button_bg(vg, hit, false, true);
                }
                ui::label(vg, name, x, cy, 13,
                          ui::rgb(last ? ui::ink : 0x495057), last);
                if (p && !last) {
                    add_hit(hit, [this, p] { fly_to(p); });
                }
                x += widths[i];
                if (!last) {
                    ui::label(vg, "›", x + 11, cy, 14, ui::rgb(ui::muted), false,
                              NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    x += 22;
                }
            }
            nvgRestore(vg);
        }
    }

    // Save status + git commit, top right.
    {
        Rect r{win_w_ - 16.0 - 150, 16, 150, 44};
        ui::island(vg, r);
        add_hit(r, nullptr);
        ui::label(vg, save_status_, r.x + 14, r.center().y, 12.5f, ui::rgb(ui::muted));
        Rect b{r.right() - 4 - 72, r.y + 6, 72, 32};
        bool hovered = b.contains(mouse_);
        ui::button_bg(vg, b, false, hovered);
        ui::icon(vg, ui::Icon::Git, {b.x + 2, b.y, 28, 32}, ui::rgb(ui::ink));
        ui::label(vg, "Commit", b.x + 28, b.center().y, 12.5f, ui::rgb(ui::ink), true);
        add_hit(b, [this] {
            flush();
            toast(store::git_commit(dir_));
        });
    }

    {
        Rect r{win_w_ - 16.0 - 150 - 8 - 44, 16, 44, 44};
        ui::island(vg, r);
        add_hit(r, nullptr);
        button({r.x + 6, r.y + 6, 32, 32}, ui::Icon::Search, search_open_,
               [this] { search_open_ ? close_search() : open_search(); });
    }

    if (style_panel_visible()) {
        draw_style_panel();
    }

    // Zoom level + undo/redo, bottom left.
    {
        double y = win_h_ - 16.0 - 40;
        Rect z{16, y, 156, 40};
        ui::island(vg, z);
        add_hit(z, nullptr);
        Vec2 center{win_w_ / 2.0, win_h_ / 2.0};
        button({z.x + 4, z.y + 4, 32, 32}, ui::Icon::Minus, false,
               [this, center] { cam_.zoom_at(center, 1 / 1.6); });
        ui::label(vg, std::format("Level {}", depth(anchor_)), z.center().x,
                  z.center().y, 12.5f, ui::rgb(ui::ink), true,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        add_hit({z.x + 40, z.y, 76, 40}, [this] { fly_out(); });
        button({z.right() - 36, z.y + 4, 32, 32}, ui::Icon::Plus, false,
               [this, center] { cam_.zoom_at(center, 1.6); });

        Rect u{z.right() + 8, y, 76, 40};
        ui::island(vg, u);
        add_hit(u, nullptr);
        button({u.x + 4, u.y + 4, 32, 32}, ui::Icon::Undo, false, [this] { undo(); });
        button({u.x + 40, u.y + 4, 32, 32}, ui::Icon::Redo, false, [this] { redo(); });
    }

    draw_search();

    // Hint, bottom right.
    if (editing_) {
        static const char* names[] = {"-- INSERT --", "-- NORMAL --", "-- VISUAL --",
                                      "-- VISUAL LINE --"};
        std::string mode = names[int(vim_.mode)];
        if (!vim_.pending().empty()) {
            mode += "   " + vim_.pending();
        }
        ui::label(vg, mode, win_w_ / 2.0, win_h_ - 28.0, 13, ui::rgb(ui::ink), true,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    }
    ui::label(vg, "Double-click or Enter dives in  ·  Esc zooms out  ·  ```py in a note makes a code block",
              win_w_ - 20.0, win_h_ - 36.0, 12, ui::rgb(0xadb5bd), false,
              NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);

    // Toast, bottom center.
    if (now_ < toast_until_ && !toast_.empty()) {
        nvgFontFace(vg, "ui-medium");
        nvgFontSize(vg, 13);
        float b[4];
        double tw2 = nvgTextBounds(vg, 0, 0, toast_.c_str(), nullptr, b) + 32;
        Rect r{(win_w_ - tw2) / 2, win_h_ - 16.0 - 40, tw2, 40};
        nvgBeginPath(vg);
        nvgRoundedRect(vg, float(r.x), float(r.y), float(r.w), float(r.h), 8);
        nvgFillColor(vg, ui::rgb(0x1b1b1f, 235));
        nvgFill(vg);
        ui::label(vg, toast_, r.center().x, r.center().y, 13, nvgRGB(255, 255, 255),
                  true, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    }
}
