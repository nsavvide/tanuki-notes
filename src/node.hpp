#pragma once

#include "geom.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class Kind {
    Root,
    Rect,
    Ellipse,
    Diamond,
    Text,
    Line,
    Arrow,
    Draw,
    Image,
    Pdf,     // container holding PdfPage children
    PdfPage, // one rendered page of the Pdf parent's file
};

enum class FillStyle { Hachure, CrossHatch, Solid };

// Colors are 0xRRGGBBAA.
struct Style {
    uint32_t stroke = 0x1e1e1eff;
    uint32_t fill = 0x00000000; // alpha 0 = transparent
    FillStyle fill_style = FillStyle::Hachure;
    double stroke_width = 0; // in PARENT local units
    int roughness = 1;       // 0 architect, 1 artist, 2 cartoonist
    double font_size = 0;    // in PARENT local units
};

struct Node {
    std::string id;
    Kind kind = Kind::Rect;
    double x = 0, y = 0, w = 1, h = 1; // bbox in the PARENT's local units
    Style style;
    std::string text;         // text node content, or a shape's label
    std::vector<Vec2> points; // Line/Arrow/Draw: normalized 0..1 in bbox
    std::string asset;        // Image/Pdf: path relative to the map folder
    int page = 0;             // PdfPage: page index
    uint32_t seed = 1;        // keeps the hand-drawn wobble stable

    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    double aspect() const { return h / w; }
    Rect rect() const { return {x, y, w, h}; }

    // Can hold children (and be zoomed into).
    bool is_container() const {
        return kind == Kind::Root || kind == Kind::Rect ||
               kind == Kind::Ellipse || kind == Kind::Diamond ||
               kind == Kind::Pdf;
    }
    bool is_linear() const {
        return kind == Kind::Line || kind == Kind::Arrow || kind == Kind::Draw;
    }
};

std::string new_id();
const char* kind_name(Kind k);

std::unique_ptr<Node> make_node(Kind kind);
Node& add_child(Node& parent, std::unique_ptr<Node> child);
std::unique_ptr<Node> detach(Node& node);
int index_in_parent(const Node& node);
Node* find_by_id(Node& root, const std::string& id);
bool is_ancestor(const Node* ancestor, const Node* node);
int depth(const Node* node);

// First line of the text, or the kind name.
std::string display_name(const Node& node);

// Serialization. `recursive` includes children under "children".
nlohmann::json node_to_json(const Node& node, bool recursive);
std::unique_ptr<Node> node_from_json(const nlohmann::json& j);

// Root of an empty map.
std::unique_ptr<Node> make_root();
