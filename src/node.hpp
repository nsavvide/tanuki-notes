#pragma once

#include <SDL3/SDL_pixels.h>
#include <memory>
#include <utility>
#include <vector>

struct Node {
    double x, y, w, h; // rect in the PARENT's local units
    SDL_Color color;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    Node(double x, double y, double w, double h, SDL_Color color)
        : x(x), y(y), w(w), h(h), color(color) {}
};

inline Node& add_child(Node& parent, double x, double y, double w, double h,
                       SDL_Color color) {
    auto child = std::make_unique<Node>(x, y, w, h, color);
    child->parent = &parent;
    parent.children.push_back(std::move(child));
    return *parent.children.back();
}

// Fills `node` with a 3x3 grid of children; only the center one recurses.
// Every node keeps the same aspect ratio, so its local space is always
// 1 wide and `aspect` high.
inline void fill_test_grid(Node& node, int depth, double aspect) {
    if (depth == 0) {
        return;
    }

    const SDL_Color dark{0, 52, 60, 255};
    const SDL_Color light{225, 235, 236, 255};
    SDL_Color color = depth % 2 == 0 ? dark : light;

    const double padding = 0.02;
    double w = 1.0 / 3.0 - 2 * padding;
    double h = w * aspect;

    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            double x = i / 3.0 + padding;
            double y = j * aspect / 3.0 + (aspect / 3.0 - h) / 2;
            Node& child = add_child(node, x, y, w, h, color);
            if (i == 1 && j == 1) {
                fill_test_grid(child, depth - 1, aspect);
            }
        }
    }
}

// Root lives in camera space (400 x 300 world units).
inline std::unique_ptr<Node> make_test_tree(int depth) {
    auto root =
        std::make_unique<Node>(0, 0, 400, 300, SDL_Color{0, 52, 60, 255});
    fill_test_grid(*root, depth, root->h / root->w);
    return root;
}
