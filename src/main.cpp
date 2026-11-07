#include "node.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <print>
#include <string>

struct Camera {
    double x = 0, y = 0, scale = 1; // top left
    double to_screen_x(double world_x) const { return (world_x - x) * scale; }
    double to_screen_y(double world_y) const { return (world_y - y) * scale; }
    double to_world_x(double screen_x) const { return screen_x / scale + x; }
    double to_world_y(double screen_y) const { return screen_y / scale + y; }
};

// Where a node's local space lands on screen:
// local (qx, qy) -> screen (ox + qx * s, oy + qy * s)
struct Xform {
    double ox, oy, s;
};

// The anchor's placement comes straight from the camera.
Xform anchor_xform(const Camera& camera) {
    return {camera.to_screen_x(0), camera.to_screen_y(0), camera.scale};
}

// Parent placement + child's fractions -> child placement.
Xform child_xform(Xform parent, const Node& child) {
    return {parent.ox + child.x * parent.s, parent.oy + child.y * parent.s,
            parent.s * child.w};
}

void draw(SDL_Renderer* renderer, const Node& node, Xform t, int window_w,
          int window_h) {
    double height = node.h / node.w * t.s;

    // Culling: children sit inside their parent, so if this box is
    // off-screen or smaller than a pixel, its whole subtree is too.
    bool off_screen = t.ox + t.s < 0 || t.ox > window_w || t.oy + height < 0 ||
                      t.oy > window_h;
    if (off_screen || t.s < 1) {
        return;
    }

    SDL_FRect rect{float(t.ox), float(t.oy), float(t.s), float(height)};
    SDL_SetRenderDrawColor(renderer, node.color.r, node.color.g, node.color.b,
                           node.color.a);
    SDL_RenderFillRect(renderer, &rect);

    for (const auto& child : node.children) {
        draw(renderer, *child, child_xform(t, *child), window_w, window_h);
    }
}

bool covers_window(const Node& node, Xform t, int window_w, int window_h) {
    double left = t.ox;
    double right = t.ox + t.s;
    double top = t.oy;
    double bottom = t.oy + node.h / node.w * t.s;

    return left <= 0 && right >= window_w && top <= 0 && bottom >= window_h;
}

int depth(const Node* node) {
    int depth = 0;
    while (node->parent) {
        depth++;
        node = node->parent;
    }

    return depth;
}

// Keeps `anchor` = the deepest node that covers the whole window, and keeps
// the camera's numbers in that node's local units. This is what makes zoom
// depth unlimited: the camera never measures anything from the root.
void reroot(Camera& camera, Node*& anchor, int window_w, int window_h) {
    // Up: the anchor no longer fills the window -> hand over to its parent.
    while (anchor->parent &&
           !covers_window(*anchor, anchor_xform(camera), window_w, window_h)) {
        const Node& a = *anchor;
        camera.x = camera.x * a.w + a.x;
        camera.y = camera.y * a.w + a.y;
        camera.scale /= a.w;
        anchor = a.parent;
    }

    // Down: a child fills the window -> hand over to that child.
    bool moved = true;
    while (moved) {
        moved = false;
        Xform t = anchor_xform(camera);
        for (const auto& child : anchor->children) {
            const Node& c = *child;
            if (covers_window(c, child_xform(t, c), window_w, window_h)) {
                camera.x = (camera.x - c.x) / c.w;
                camera.y = (camera.y - c.y) / c.w;
                camera.scale *= c.w;
                anchor = child.get();
                moved = true;
                break; // anchor changed: don't keep iterating old children
            }
        }
    }
}

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return 1;
    }

    SDL_Window* window =
        SDL_CreateWindow("tanuki-notes", 1280, 800, SDL_WINDOW_RESIZABLE);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    SDL_SetRenderVSync(renderer, 1);

    auto root = make_test_tree(100);

    Camera camera{0, 0, 400};
    Node* anchor = root.get();
    std::string last_title;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT: {
                running = false;
                break;
            }
            case SDL_EVENT_MOUSE_MOTION: {
                if (event.motion.state & SDL_BUTTON_LMASK) {
                    camera.x -= event.motion.xrel / camera.scale;
                    camera.y -= event.motion.yrel / camera.scale;
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {
                // keep the world point under the cursor fixed whileooming
                double mouse_x = event.wheel.mouse_x;
                double mouse_y = event.wheel.mouse_y;

                double world_x = camera.to_world_x(mouse_x),
                       world_y = camera.to_world_y(mouse_y);

                // hard zoom limits. Max only matters when zooming
                // into a leaf or an empty gap, where no child takes over.
                camera.scale = std::clamp(
                    camera.scale * std::pow(1.15, event.wheel.y), 10.0, 1e9);
                camera.x = world_x - mouse_x / camera.scale;
                camera.y = world_y - mouse_y / camera.scale;

                break;
            }
            }
        }

        // every frame: Hyprland resizes the window after creation
        int window_w, window_h;
        SDL_GetWindowSize(window, &window_w, &window_h);

        reroot(camera, anchor, window_w, window_h);

        SDL_SetRenderDrawColor(renderer, 250, 250, 250, 255);
        SDL_RenderClear(renderer);
        draw(renderer, *anchor, anchor_xform(camera), window_w, window_h);

        std::string title = std::format("depth: {} | scale {:.0f}",
                                        depth(anchor), camera.scale);
        if (title != last_title) {
            SDL_SetWindowTitle(window, title.c_str());
            std::println("{}", title);
            last_title = title;
        }

        SDL_RenderPresent(renderer);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
