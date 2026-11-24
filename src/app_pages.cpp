// The map's Markdown pages (pages.hpp) and the node tree, kept in step both
// ways: pages win when they change on disk; the tree is written back on save.

#include "app.hpp"
#include "code.hpp"
#include "layout.hpp"
#include "pages.hpp"
#include "render.hpp"
#include "ui.hpp"

#include <nanovg.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <print>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

// A labelled shape is a box: it has a page (or a `##` section).
bool is_box(const Node& n) {
    return (n.kind == Kind::Rect || n.kind == Kind::Ellipse || n.kind == Kind::Diamond) &&
           !n.text.empty();
}
bool is_note(const Node& n) { return n.kind == Kind::Text || n.kind == Kind::Code; }
// Arrows between two boxes are written as `-> [[Box]]` (flow steps aren't).
bool is_link(const Node& n) {
    return (n.kind == Kind::Arrow || n.kind == Kind::Line) && n.tags.empty() &&
           !n.start_bind.empty() && !n.end_bind.empty();
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = char(std::tolower((unsigned char)c));
    }
    return s;
}
std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }

std::string op_of(const Node& a) {
    if (a.kind == Kind::Line) {
        return a.style.dash == 2 ? ".." : "--";
    }
    return a.style.start_arrow   ? "<->"
           : a.style.dash == 1   ? "-->"
           : a.style.dash == 2   ? "..>"
                                 : "->";
}

void set_op(Node& a, const std::string& op) {
    if (op_of(a) == op) {
        return;
    }
    a.kind = op == "--" || op == ".." ? Kind::Line : Kind::Arrow;
    a.style.dash = op == "-->" ? 1 : op == "..>" || op == ".." ? 2 : 0;
    a.style.start_arrow = op == "<->";
}

void boxes_of(Node& n, std::vector<Node*>& out) {
    for (auto& c : n.children) {
        if (is_box(*c)) {
            out.push_back(c.get());
            boxes_of(*c, out);
        }
    }
}

// Titles from `b` up through the boxes holding it.
std::vector<std::string> title_path(const Node* b) {
    std::vector<std::string> t;
    for (; b && is_box(*b); b = b->parent) {
        t.push_back(lower(first_line(b->text)));
    }
    return t; // innermost first
}

// Does "a > b > c" name `b`? The last part is its title, earlier parts are
// boxes further up (not necessarily its direct parent).
bool names(const Node* b, const std::vector<std::string>& segs) {
    auto path = title_path(b);
    if (segs.empty() || path.empty() || path[0] != segs.back()) {
        return false;
    }
    size_t k = 1;
    for (size_t i = segs.size() - 1; i-- > 0;) {
        while (k < path.size() && path[k] != segs[i]) {
            ++k;
        }
        if (k++ >= path.size()) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> split_ref(const std::string& ref) {
    std::vector<std::string> segs;
    size_t p = 0;
    while (true) {
        size_t q = ref.find(" > ", p);
        std::string s = ref.substr(p, q - p);
        size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
        segs.push_back(lower(a == std::string::npos ? "" : s.substr(a, b - a + 1)));
        if (q == std::string::npos) {
            return segs;
        }
        p = q + 3;
    }
}

// Depth of the deepest shape holding both.
int shared_depth(const Node* a, const Node* b) {
    for (const Node* p = b; p; p = p->parent) {
        if (is_ancestor(p, a)) {
            return depth(p);
        }
    }
    return 0;
}

// The box a [[ref]] names; when several do, the one nearest to `from`.
Node* resolve(const std::string& ref, const Node* from, const std::vector<Node*>& boxes) {
    auto segs = split_ref(ref);
    Node* best = nullptr;
    int best_d = std::numeric_limits<int>::min();
    for (Node* b : boxes) {
        if (names(b, segs)) {
            int d = from ? shared_depth(from, b) : -depth(b);
            if (d > best_d) {
                best = b;
                best_d = d;
            }
        }
    }
    return best;
}

// The shortest "a > b" that names `b` and nothing else.
// ponytail: O(boxes) per ref; index titles if maps get to thousands of boxes.
std::string ref_of(const Node* b, const std::vector<Node*>& boxes) {
    std::vector<std::string> titles;
    for (const Node* p = b; p && is_box(*p); p = p->parent) {
        titles.push_back(first_line(p->text));
    }
    std::string ref;
    for (size_t k = 1; k <= titles.size(); ++k) {
        std::vector<std::string> segs;
        ref.clear();
        for (size_t i = k; i-- > 0;) {
            segs.push_back(lower(titles[i]));
            ref += (ref.empty() ? "" : " > ") + titles[i];
        }
        if (std::count_if(boxes.begin(), boxes.end(), [&](Node* x) { return names(x, segs); }) == 1) {
            return ref;
        }
    }
    return ref;
}

Node* common_parent(Node* a, Node* b) {
    for (Node* p = a->parent; p; p = p->parent) {
        if (is_ancestor(p, b)) {
            return p;
        }
    }
    return nullptr;
}

// Which boxes get their own page file. Kept as they were; a box seen for the
// first time gets a folder when it holds boxes and its parent has a folder
// too, otherwise it's a `##` section of its parent's page.
void assign_files(Node& n, const std::string& folder, bool has_folder) {
    std::set<std::string> used;
    std::vector<Node*> todo;
    for (auto& cp : n.children) {
        Node& c = *cp;
        if (!is_box(c)) {
            continue;
        }
        bool file = c.mdfile.ends_with(".md");
        if (file && has_folder) {
            fs::path p(c.mdfile);
            bool idx = p.filename() == "_index.md";
            std::string dir = (idx ? p.parent_path().parent_path() : p.parent_path()).generic_string();
            std::string name = idx ? p.parent_path().filename().string() : p.stem().string();
            if ((dir.empty() ? "" : dir + "/") == folder && !used.contains(name)) {
                used.insert(name);
                continue;
            }
        }
        todo.push_back(&c);
    }
    for (Node* c : todo) {
        bool was_file = c->mdfile.ends_with(".md");
        bool holds_boxes = std::any_of(c->children.begin(), c->children.end(),
                                       [](auto& k) { return is_box(*k); });
        if (!has_folder || !(was_file || (c->mdfile.empty() && holds_boxes))) {
            c->mdfile = "#";
            continue;
        }
        std::string name = pages::slug(c->text);
        for (int i = 2; used.contains(name); ++i) {
            name = pages::slug(c->text) + "-" + std::to_string(i);
        }
        used.insert(name);
        bool idx = c->mdfile.empty() || c->mdfile.ends_with("/_index.md") || c->mdfile == "_index.md";
        c->mdfile = folder + name + (idx ? "/_index.md" : ".md");
    }
    for (auto& cp : n.children) {
        if (is_box(*cp)) {
            bool f = cp->mdfile.ends_with("/_index.md");
            assign_files(*cp, f ? fs::path(cp->mdfile).parent_path().generic_string() + "/" : "", f);
        }
    }
}

} // namespace

void App::load_pages(bool startup) {
    // Only a map reads its pages: a folder with a README isn't one yet.
    if (!fs::exists(dir_ / "board.json") && !fs::exists(dir_ / "_index.md")) {
        return;
    }
    pages::Map m = pages::read(dir_);
    page_stamps_ = pages::stamps(dir_);
    // Only pages read() parsed are ours to delete. stamps() also lists .md
    // files read() skips (a folder without _index.md, e.g. daily/), and those
    // must never be deleted.
    std::function<void(const pages::Box&)> known = [&](const pages::Box& b) {
        if (!b.file.empty()) {
            known_pages_.insert(b.file);
        }
        for (const auto& k : b.kids) {
            known(k);
        }
    };
    known(m.root);
    flows_ = m.flows;
    std::string anchor_id = anchor_->id;
    selection_.clear();
    std::vector<std::string> problems;

    std::vector<std::pair<Node*, const pages::Box*>> owners;
    std::function<void(Node&, const pages::Box&)> apply = [&](Node& n, const pages::Box& b) {
        if (n.kind != Kind::Root) {
            n.text = b.title;
            n.mdfile = b.file.empty() ? "#" : b.file;
            n.md = true;
        }
        n.issues = b.issues;
        owners.push_back({&n, &b});

        // Boxes match by title, in order; pages win.
        std::vector<Node*> old;
        for (auto& c : n.children) {
            if (c->md && is_box(*c)) {
                old.push_back(c.get());
            }
        }
        std::vector<Node*> kids;
        for (const auto& kb : b.kids) {
            auto it = std::find_if(old.begin(), old.end(),
                                   [&](Node* o) { return o && o->text == kb.title; });
            Node* k;
            if (it != old.end()) {
                k = *it;
                *it = nullptr;
            } else {
                auto fresh = make_node(Kind::Rect);
                fresh->w = 0; // placed by layout::place_new
                k = &add_child(n, std::move(fresh));
            }
            kids.push_back(k);
        }
        for (Node* o : old) {
            if (o) {
                detach(*o); // gone from the pages
            }
        }
        for (size_t i = 0; i < kids.size(); ++i) {
            apply(*kids[i], b.kids[i]);
        }

        // Notes match by position.
        std::vector<Node*> notes;
        for (auto& c : n.children) {
            if (c->md && is_note(*c)) {
                notes.push_back(c.get());
            }
        }
        for (size_t i = 0; i < b.notes.size(); ++i) {
            const pages::Note& pn = b.notes[i];
            Node* note = i < notes.size() ? notes[i] : nullptr;
            Kind kind = pn.code ? Kind::Code : Kind::Text;
            if (note && note->kind != kind) {
                detach(*note);
                note = nullptr;
            }
            if (!note) {
                auto fresh = make_node(kind);
                fresh->w = 0;
                note = &add_child(n, std::move(fresh));
            }
            std::string lang = pn.code ? code::from_fence(pn.lang) : "";
            if (note->text != pn.text || note->lang != lang) {
                note->text = pn.text;
                note->lang = lang;
                if (note->w > 0) {
                    double old_h = note->h;
                    if (kind == Kind::Text) {
                        fit_text_bounds(vg_, *note);
                    } else {
                        fit_code_bounds(vg_, *note);
                    }
                    if (note->h > old_h) { // longer now: push what's below it down
                        double new_h = note->h;
                        note->h = old_h;
                        layout::grow(*note, new_h);
                    }
                }
            }
            note->md = true;
        }
        for (size_t i = b.notes.size(); i < notes.size(); ++i) {
            detach(*notes[i]); // gone from the page
        }
    };

    std::vector<Node*> boxes;
    std::unordered_map<std::string, Node*> by_id;
    auto index = [&] {
        boxes.clear();
        boxes_of(*root_, boxes);
        by_id.clear();
        std::function<void(Node&)> walk = [&](Node& n) {
            by_id[n.id] = &n;
            for (auto& c : n.children) {
                walk(*c);
            }
        };
        walk(*root_);
    };

    if (m.any) {
        apply(*root_, m.root);
        index();
        // Arrows: `-> [[Box]]` lines, matched by target in order.
        std::map<Node*, std::vector<Node*>> old_links;
        for (auto& [id, n] : by_id) {
            if (n->md && is_link(*n) && by_id.contains(n->start_bind)) {
                old_links[by_id[n->start_bind]].push_back(n);
            }
        }
        for (auto& [owner, links] : old_links) {
            std::sort(links.begin(), links.end(), [](Node* a, Node* b) { return a->id < b->id; });
        }
        std::vector<Node*> drop;
        for (auto [owner, b] : owners) {
            auto& old = old_links[owner];
            for (const auto& l : b->links) {
                Node* to = resolve(l.target, owner, boxes);
                std::string from = owner->text.empty() ? "top level" : first_line(owner->text);
                if (!to || to == owner || owner->kind == Kind::Root) {
                    problems.push_back(std::format("{}: no box called [[{}]]", from, l.target));
                    continue;
                }
                if (is_ancestor(owner, to) || is_ancestor(to, owner)) {
                    // It would start or end inside its own box and float there.
                    problems.push_back(std::format("{}: [[{}]] is inside it (or holds it); arrows join "
                                                   "boxes side by side",
                                                   from, l.target));
                    continue;
                }
                auto it = std::find_if(old.begin(), old.end(),
                                       [&](Node* a) { return a && a->end_bind == to->id; });
                Node* a;
                if (it != old.end()) {
                    a = *it;
                    *it = nullptr;
                } else if (Node* host = common_parent(owner, to)) {
                    auto fresh = make_node(l.op == "--" || l.op == ".." ? Kind::Line : Kind::Arrow);
                    fresh->start_bind = owner->id;
                    fresh->end_bind = to->id;
                    a = &add_child(*host, std::move(fresh)); // points from layout
                } else {
                    continue;
                }
                a->text = l.label;
                set_op(*a, l.op);
                a->md = true;
            }
            for (Node* a : old) {
                if (a) {
                    drop.push_back(a);
                }
            }
        }
        // Also arrows whose box is gone.
        for (auto& [id, n] : by_id) {
            if (n->md && is_link(*n) &&
                (!by_id.contains(n->start_bind) || !by_id.contains(n->end_bind))) {
                drop.push_back(n);
            }
        }
        for (Node* a : drop) {
            if (a->parent) {
                detach(*a);
            }
        }
    }

    // Flows: numbered arrows tagged with the flow, kept while they match.
    index();
    std::map<std::string, Node*> old_steps;
    for (auto& [id, n] : by_id) {
        if (n->mdfile.starts_with("flow:")) {
            old_steps[n->mdfile] = n;
        }
    }
    std::set<std::string> kept;
    for (const auto& f : flows_) {
        for (size_t i = 0; i < f.steps.size(); ++i) {
            const auto& s = f.steps[i];
            Node* from = resolve(s.from, nullptr, boxes);
            Node* to = resolve(s.to, nullptr, boxes);
            if (!from || !to || from == to) {
                problems.push_back(std::format("flows/{}.md: no box called [[{}]]", f.id,
                                               from ? s.to : s.from));
                continue;
            }
            std::string key = std::format("flow:{}#{}", f.id, i + 1);
            Node* a = old_steps.contains(key) ? old_steps[key] : nullptr;
            if (a && (a->start_bind != from->id || a->end_bind != to->id)) {
                a = nullptr; // a different step now: make it anew
            }
            if (!a) {
                Node* host = common_parent(from, to);
                if (!host) {
                    continue;
                }
                auto fresh = make_node(Kind::Arrow);
                fresh->id = pages::hash_id(key + from->id + to->id);
                fresh->seed = uint32_t(std::hash<std::string>{}(fresh->id)) | 1u;
                fresh->start_bind = from->id;
                fresh->end_bind = to->id;
                fresh->mdfile = key;
                fresh->tags = {f.id};
                if (old_steps.contains(key)) {
                    detach(*old_steps[key]);
                    old_steps.erase(key);
                }
                a = &add_child(*host, std::move(fresh));
            }
            a->text = s.label.empty() ? std::to_string(i + 1) : std::format("{}. {}", i + 1, s.label);
            set_op(*a, s.op);
            kept.insert(key);
        }
    }
    for (auto& [key, n] : old_steps) {
        if (!kept.contains(key)) {
            detach(*n);
        }
    }

    layout::place_new(vg_, *root_);

    anchor_ = find_by_id(*root_, anchor_id);
    if (!anchor_) { // we were inside a box that's gone
        anchor_ = root_.get();
        cam_.scale = 1e6;
        cam_.x = 0.5 - win_w_ / 2.0 / cam_.scale;
        cam_.y = 0.5 - win_h_ / 2.0 / cam_.scale;
        fly_to_content();
    }
    pages_error_.clear();
    for (const auto& p : problems) {
        pages_error_ += (pages_error_.empty() ? "" : "\n") + p;
        std::println(stderr, "tanuki-notes: {}", p);
    }
    if (!problems.empty()) {
        toast(problems[0] + (problems.size() > 1 ? std::format(" (+{} more)", problems.size() - 1) : ""));
    }
    if (startup) {
        mark_dirty();
    } else {
        commit();
    }
}

void App::save_pages() {
    if (!fs::exists(dir_ / "board.json") && !fs::exists(dir_ / "_index.md") &&
        root_->children.empty()) {
        return; // just looking around a folder that isn't a map
    }
    assign_files(*root_, "", true);
    std::vector<Node*> boxes;
    boxes_of(*root_, boxes);
    std::set<const Node*> in_pages(boxes.begin(), boxes.end());
    std::unordered_map<std::string, Node*> by_id;
    std::vector<Node*> links;
    std::function<void(Node&)> walk = [&](Node& n) {
        by_id[n.id] = &n;
        if (is_link(n)) {
            links.push_back(&n);
        }
        for (auto& c : n.children) {
            walk(*c);
        }
    };
    walk(*root_);
    std::map<const Node*, std::vector<Node*>> out;
    for (Node* a : links) {
        auto s = by_id.find(a->start_bind), e = by_id.find(a->end_bind);
        if (s != by_id.end() && e != by_id.end() && in_pages.contains(s->second) &&
            in_pages.contains(e->second)) {
            out[s->second].push_back(a);
            a->md = true;
        }
    }

    std::function<pages::Box(Node&)> spec = [&](Node& n) {
        pages::Box b;
        bool root = n.kind == Kind::Root;
        b.title = root ? "" : n.text;
        b.file = root ? "_index.md" : n.mdfile == "#" ? "" : n.mdfile;
        b.issues = n.issues;
        n.md = !root;
        for (auto& c : n.children) {
            if (is_box(*c)) {
                b.kids.push_back(spec(*c));
            } else if (is_note(*c)) {
                b.notes.push_back({c->kind == Kind::Code, c->text, c->lang});
                c->md = true;
            }
        }
        for (Node* a : out[&n]) {
            b.links.push_back({op_of(*a), ref_of(by_id[a->end_bind], boxes), a->text});
        }
        return b;
    };
    auto files = pages::render(spec(*root_));
    pages::write(dir_, files, known_pages_);
    known_pages_.clear();
    for (const auto& [f, _] : files) {
        known_pages_.insert(f);
    }
}

void App::watch_pages() {
    last_scan_ = now_;
    if (editing_ || drag_ != Drag::None) {
        return; // pointers into the tree are live; look again in a moment
    }
    auto st = pages::stamps(dir_);
    if (st == page_stamps_) {
        return;
    }
    load_pages(false);
    toast("Updated from the pages");
}

std::vector<App::FocusItem> App::focus_items() const {
    std::vector<FocusItem> out;
    int k = 0;
    for (const auto& f : flows_) {
        out.push_back({"flow:" + f.id, f.title, f.note,
                       ui::shade(ui::auto_order[k++ % ui::n_hues], 3)});
    }
    std::set<std::string> seen;
    std::function<void(const Node&)> walk = [&](const Node& n) {
        for (const auto& i : n.issues) {
            std::string url = pages::issue_url(i);
            if (seen.insert(url).second) {
                out.push_back({"issue:" + url, pages::issue_title(i), url,
                               ui::shade(ui::auto_order[k++ % ui::n_hues], 3)});
            }
        }
        for (const auto& c : n.children) {
            walk(*c);
        }
    };
    walk(*root_);
    return out;
}

void App::cycle_focus() {
    auto list = focus_items();
    size_t i = 0;
    while (i < list.size() && list[i].key != focus_) {
        ++i;
    }
    // none -> first -> ... -> last -> none
    focus_ = focus_.empty() ? (list.empty() ? "" : list[0].key)
             : i + 1 < list.size() ? list[i + 1].key
                                   : "";
    for (const auto& f : list) {
        if (f.key == focus_) {
            toast(f.name);
        }
    }
}

// Lights up the focused flow (its arrows and the boxes they join) or issue
// (the boxes linked to it), and every shape on the way down to them.
void App::highlight(RenderCtx& ctx) const {
    if (focus_.empty()) {
        return;
    }
    for (const auto& f : focus_items()) {
        if (f.key == focus_) {
            ctx.glow_color = f.color;
        }
    }
    bool flow = focus_.starts_with("flow:");
    std::string key = focus_.substr(flow ? 5 : 6);
    std::set<std::string> ends;
    std::function<void(const Node&)> mark = [&](const Node& n) {
        bool on = flow ? std::find(n.tags.begin(), n.tags.end(), key) != n.tags.end()
                       : std::any_of(n.issues.begin(), n.issues.end(),
                                     [&](auto& i) { return pages::issue_url(i) == key; });
        if (on) {
            ctx.glow.insert(&n);
            if (flow) {
                ends.insert(n.start_bind);
                ends.insert(n.end_bind);
            }
        }
        for (const auto& c : n.children) {
            mark(*c);
        }
    };
    mark(*root_);
    std::function<bool(const Node&, bool)> lit = [&](const Node& n, bool inside) {
        if (ends.contains(n.id)) {
            ctx.glow.insert(&n);
        }
        bool on = ctx.glow.contains(&n);
        bool any = on || inside;
        for (const auto& c : n.children) {
            any |= lit(*c, inside || on);
        }
        if (any) {
            ctx.lit.insert(&n);
        }
        return any;
    };
    lit(*root_, false);
    ctx.focus = true;
}

// Flows and issues, top right: click one (or press C) to light it up.
void App::draw_focus_panel() {
    auto list = focus_items();
    if (list.empty()) {
        return;
    }
    NVGcontext* vg = vg_;
    double w = 260, x = win_w_ - 16.0 - w, y = 72, row = 30;
    const FocusItem* active = nullptr;
    for (const auto& f : list) {
        if (f.key == focus_) {
            active = &f;
        }
    }
    std::string note = active ? active->note : "";
    std::erase_if(note, [](char c) { return c == '*' || c == '`'; }); // plain text here
    float note_h = 0;
    if (!note.empty()) {
        nvgFontFace(vg, "ui");
        nvgFontSize(vg, 12.5f);
        nvgTextLineHeight(vg, 1.35f);
        float b[4];
        nvgTextBoxBounds(vg, 0, 0, float(w - 24), note.c_str(), nullptr, b);
        note_h = b[3] - b[1] + 12;
    }
    size_t shown = std::min<size_t>(list.size(), 14);
    bool issues_header = std::any_of(list.begin(), list.begin() + long(shown),
                                     [](auto& f) { return f.key.starts_with("issue:"); });
    bool flows_header = list[0].key.starts_with("flow:");
    Rect panel{x, y,
               w, 12 + (flows_header + issues_header) * 22.0 + shown * row + 6 + note_h};
    ui::island(vg, panel);
    add_hit(panel, nullptr);
    double ry = y + 8;
    bool in_issues = false;
    if (flows_header) {
        ui::label(vg, "Flows  (C)", x + 12, ry + 10, 11.5f, ui::rgb(0x495057));
        ry += 22;
    }
    for (size_t i = 0; i < shown; ++i) {
        const auto& f = list[i];
        if (!in_issues && f.key.starts_with("issue:")) {
            in_issues = true;
            ui::label(vg, flows_header ? "Issues" : "Issues  (C)", x + 12, ry + 10, 11.5f,
                      ui::rgb(0x495057));
            ry += 22;
        }
        Rect r{x + 6, ry, w - 12, row - 2};
        bool on = &f == active;
        ui::button_bg(vg, r, on, r.contains(mouse_));
        nvgBeginPath(vg);
        nvgCircle(vg, float(r.x + 14), float(r.center().y), 5);
        ui::fill(vg, nvgRGBA((f.color >> 24) & 0xff, (f.color >> 16) & 0xff,
                             (f.color >> 8) & 0xff, 255));
        nvgFill(vg);
        std::string name = f.name.size() > 30 ? f.name.substr(0, 29) + "…" : f.name;
        ui::label(vg, name, r.x + 28, r.center().y, 13, ui::rgb(on ? ui::primary : ui::ink), on);
        std::string key = f.key;
        add_hit(r, [this, key] { focus_ = focus_ == key ? "" : key; });
        ry += row;
    }
    if (note_h > 0) {
        nvgFontFace(vg, "ui");
        nvgFontSize(vg, 12.5f);
        nvgTextLineHeight(vg, 1.35f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        ui::fill(vg, ui::rgb(0x495057));
        nvgTextBox(vg, float(x + 12), float(ry + 8), float(w - 24), note.c_str(), nullptr);
    }
}

// A small tag per linked issue in the top right corner of a box; click to
// open it in the browser.
void App::draw_issue_badges(Node* top, Xform t) {
    NVGcontext* vg = vg_;
    Rect view{0, 0, double(win_w_), double(win_h_)};
    int budget = 60; // ponytail: at most 60 tags per frame
    std::function<void(Node&, Xform)> walk = [&](Node& n, Xform x) {
        Rect r = screen_rect(n, x);
        if (n.kind != Kind::Root && (!view.intersects(r) || r.w < 2)) {
            return;
        }
        if (is_box(n) && !n.issues.empty() && r.w >= 90 && budget > 0) {
            double bx = r.right() - 6;
            nvgFontFace(vg, "ui-medium");
            nvgFontSize(vg, 11);
            for (const auto& i : n.issues) {
                std::string title = pages::issue_title(i);
                if (title.size() > 26) {
                    title = title.substr(0, 25) + "…";
                }
                std::string label = "↗ " + title;
                float b[4];
                double tw = nvgTextBounds(vg, 0, 0, label.c_str(), nullptr, b) + 14;
                Rect pill{bx - tw, r.y + 6, tw, 20};
                if (pill.x < r.x + 6) {
                    break;
                }
                bool hover = pill.contains(mouse_);
                nvgBeginPath(vg);
                nvgRoundedRect(vg, float(pill.x), float(pill.y), float(pill.w), float(pill.h), 10);
                ui::fill(vg, ui::rgb(hover ? ui::primary_light : 0xffffff, 235));
                nvgFill(vg);
                ui::stroke(vg, ui::rgb(ui::primary, 160));
                nvgStrokeWidth(vg, 1);
                nvgStroke(vg);
                ui::label(vg, label, pill.x + 7, pill.center().y, 11, ui::rgb(ui::primary), true);
                std::string url = pages::issue_url(i);
                add_hit(pill, [url] { SDL_OpenURL(url.c_str()); });
                bx = pill.x - 4;
                --budget;
            }
        }
        for (auto& c : n.children) {
            if (c->is_container()) {
                walk(*c, child_xform(x, *c));
            }
        }
    };
    walk(*top, t);
}
