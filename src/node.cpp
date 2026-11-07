#include "node.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <random>

using nlohmann::json;

std::string new_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    char buf[17];
    std::snprintf(buf, sizeof buf, "%012llx",
                  (unsigned long long)(rng() & 0xffffffffffffULL));
    return buf;
}

static const char* kind_names[] = {"root",  "rect",  "ellipse", "diamond",
                                   "text",  "line",  "arrow",   "draw",
                                   "image", "pdf",   "pdf-page"};

const char* kind_name(Kind k) { return kind_names[int(k)]; }

static Kind kind_from_name(const std::string& s) {
    for (int i = 0; i < int(std::size(kind_names)); ++i) {
        if (s == kind_names[i]) {
            return Kind(i);
        }
    }
    return Kind::Rect;
}

std::unique_ptr<Node> make_node(Kind kind) {
    auto n = std::make_unique<Node>();
    n->id = new_id();
    n->kind = kind;
    n->seed = uint32_t(std::hash<std::string>{}(n->id)) | 1u;
    return n;
}

Node& add_child(Node& parent, std::unique_ptr<Node> child) {
    child->parent = &parent;
    parent.children.push_back(std::move(child));
    return *parent.children.back();
}

int index_in_parent(const Node& node) {
    const auto& siblings = node.parent->children;
    for (int i = 0; i < int(siblings.size()); ++i) {
        if (siblings[i].get() == &node) {
            return i;
        }
    }
    return -1;
}

std::unique_ptr<Node> detach(Node& node) {
    auto& siblings = node.parent->children;
    auto it = siblings.begin() + index_in_parent(node);
    std::unique_ptr<Node> owned = std::move(*it);
    siblings.erase(it);
    owned->parent = nullptr;
    return owned;
}

Node* find_by_id(Node& root, const std::string& id) {
    if (root.id == id) {
        return &root;
    }
    for (auto& c : root.children) {
        if (Node* found = find_by_id(*c, id)) {
            return found;
        }
    }
    return nullptr;
}

bool is_ancestor(const Node* ancestor, const Node* node) {
    for (const Node* n = node; n; n = n->parent) {
        if (n == ancestor) {
            return true;
        }
    }
    return false;
}

int depth(const Node* node) {
    int d = 0;
    while (node->parent) {
        ++d;
        node = node->parent;
    }
    return d;
}

std::string display_name(const Node& node) {
    if (node.kind == Kind::Root) {
        return "Home";
    }
    std::string first = node.text.substr(0, node.text.find('\n'));
    if (!first.empty()) {
        return first.size() > 40 ? first.substr(0, 40) + "…" : first;
    }
    if (!node.asset.empty()) {
        auto slash = node.asset.find_last_of('/');
        return node.asset.substr(slash == std::string::npos ? 0 : slash + 1);
    }
    return kind_name(node.kind);
}

static const char* fill_names[] = {"hachure", "cross-hatch", "solid"};

static std::string color_hex(uint32_t c) {
    char buf[10];
    std::snprintf(buf, sizeof buf, "#%08x", c);
    return buf;
}

static uint32_t color_from_hex(const std::string& s) {
    return s.size() == 9 ? uint32_t(std::stoul(s.substr(1), nullptr, 16)) : 0;
}

json node_to_json(const Node& n, bool recursive) {
    json j;
    j["id"] = n.id;
    j["kind"] = kind_name(n.kind);
    j["x"] = n.x;
    j["y"] = n.y;
    j["w"] = n.w;
    j["h"] = n.h;
    if (!n.text.empty()) {
        j["text"] = n.text;
    }
    j["stroke"] = color_hex(n.style.stroke);
    j["fill"] = color_hex(n.style.fill);
    j["fill_style"] = fill_names[int(n.style.fill_style)];
    j["stroke_width"] = n.style.stroke_width;
    j["roughness"] = n.style.roughness;
    if (n.style.font_size > 0) {
        j["font_size"] = n.style.font_size;
    }
    j["seed"] = n.seed;
    if (!n.points.empty()) {
        json pts = json::array();
        for (Vec2 p : n.points) {
            pts.push_back({p.x, p.y});
        }
        j["points"] = pts;
    }
    if (!n.asset.empty()) {
        j["asset"] = n.asset;
    }
    if (n.kind == Kind::PdfPage) {
        j["page"] = n.page;
    }
    if (recursive && !n.children.empty()) {
        json kids = json::array();
        for (const auto& c : n.children) {
            kids.push_back(node_to_json(*c, true));
        }
        j["children"] = kids;
    }
    return j;
}

std::unique_ptr<Node> node_from_json(const json& j) {
    auto n = std::make_unique<Node>();
    n->id = j.value("id", new_id());
    n->kind = kind_from_name(j.value("kind", "rect"));
    n->x = j.value("x", 0.0);
    n->y = j.value("y", 0.0);
    n->w = j.value("w", 1.0);
    n->h = j.value("h", 1.0);
    n->text = j.value("text", "");
    n->style.stroke = color_from_hex(j.value("stroke", "#1e1e1eff"));
    n->style.fill = color_from_hex(j.value("fill", "#00000000"));
    std::string fs = j.value("fill_style", "hachure");
    for (int i = 0; i < 3; ++i) {
        if (fs == fill_names[i]) {
            n->style.fill_style = FillStyle(i);
        }
    }
    n->style.stroke_width = j.value("stroke_width", 0.0);
    n->style.roughness = j.value("roughness", 1);
    n->style.font_size = j.value("font_size", 0.0);
    n->seed = j.value("seed", 1u);
    if (j.contains("points")) {
        for (const auto& p : j["points"]) {
            n->points.push_back({p[0].get<double>(), p[1].get<double>()});
        }
    }
    n->asset = j.value("asset", "");
    n->page = j.value("page", 0);
    if (j.contains("children")) {
        for (const auto& c : j["children"]) {
            add_child(*n, node_from_json(c));
        }
    }
    return n;
}

std::unique_ptr<Node> make_root() {
    auto root = std::make_unique<Node>();
    root->id = "root";
    root->kind = Kind::Root;
    root->w = 1;
    root->h = 1;
    return root;
}
