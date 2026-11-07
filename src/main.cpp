#include "app.hpp"
#include "text.hpp"

#include <SDL3/SDL.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#define NANOVG_GL3_IMPLEMENTATION
#include <nanovg.h>
#include <nanovg_gl.h>
#include <nanovg_gl_utils.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <print>
#include <string>
#include <vector>

int run_selftest(NVGcontext* vg, SDL_Window* window);
void make_demo(App& app);

namespace {

std::filesystem::path default_map_dir() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / "tanuki";
}

Mods mods() {
    SDL_Keymod m = SDL_GetModState();
    return {(m & SDL_KMOD_SHIFT) != 0, (m & SDL_KMOD_CTRL) != 0,
            (m & SDL_KMOD_ALT) != 0};
}

// Renders `frames` frames of the map into an offscreen buffer and writes a
// PNG. Used to check rendering without a visible window.
int snapshot(NVGcontext* vg, SDL_Window* window, App& app, const char* out) {
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    NVGLUframebuffer* fb = nvgluCreateFramebuffer(vg, w, h, 0);
    if (!fb) {
        std::println(stderr, "framebuffer failed");
        return 1;
    }
    for (int i = 0; i < 40; ++i) {
        nvgluBindFramebuffer(fb);
        glViewport(0, 0, w, h);
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        app.update(i / 60.0, w, h);
        app.render(1);
    }
    std::vector<unsigned char> px(size_t(w) * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    nvgluBindFramebuffer(nullptr);
    SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < h; ++y) { // GL rows are bottom-up
        std::memcpy((unsigned char*)s->pixels + size_t(y) * s->pitch,
                    &px[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
    }
    bool ok = SDL_SavePNG(s, out);
    SDL_DestroySurface(s);
    nvgluDeleteFramebuffer(fb);
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    bool selftest = !args.empty() && args[0] == "--selftest";
    bool snap = args.size() >= 2 && args[0] == "--snapshot";
    bool demo = args.size() >= 2 && args[0] == "--make-demo";
    bool headless = selftest || snap || demo;

    std::filesystem::path map_dir = default_map_dir();
    if (snap && args.size() >= 3) {
        map_dir = args[2];
    } else if (demo) {
        map_dir = args[1];
    } else if (!headless && !args.empty()) {
        map_dir = args[0];
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init: {}", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (headless) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    SDL_Window* window = SDL_CreateWindow("tanuki-notes", 1280, 800, flags);
    SDL_GLContext gl = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!gl) {
        std::println(stderr, "OpenGL context: {}", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    NVGcontext* vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!vg || !text::init(vg, TANUKI_ASSET_DIR "/fonts")) {
        std::println(stderr, "NanoVG or fonts failed to load");
        return 1;
    }

    int code = 0;
    if (selftest) {
        code = run_selftest(vg, window);
    } else if (demo) {
        App app(vg, window, map_dir);
        make_demo(app);
        // Extra args: files to insert (images, PDFs) along the right side.
        for (size_t i = 2; i < args.size(); ++i) {
            app.update(0, 1280, 800);
            app.insert_file(args[i], {1150.0, 200.0 + 260.0 * double(i - 2)});
        }
        app.flush();
    } else if (snap) {
        App app(vg, window, map_dir);
        if (args.size() >= 6) { // --snapshot out dir x y zoom
            Vec2 at{std::stod(args[3]), std::stod(args[4])};
            double steps = 120, k = std::pow(std::stod(args[5]), 1 / steps);
            for (int i = 0; i < steps; ++i) {
                app.update(0, 1280, 800);
                app.camera().zoom_at(at, k);
            }
        }
        if (std::getenv("TANUKI_SNAPSHOT_SELECT")) { // show the style panel
            app.update(0, 1280, 800);
            app.key_down(SDLK_A, {false, true, false});
        }
        code = snapshot(vg, window, app, args[1].c_str());
    } else {
        App app(vg, window, map_dir);
        std::println("tanuki-notes: map folder {}", map_dir.string());
        bool running = true;
        while (running) {
            // Sleep until something happens unless an animation is running.
            if (!app.busy()) {
                SDL_WaitEventTimeout(nullptr, 500);
            }
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    app.mouse_down({e.button.x, e.button.y}, e.button.button,
                                   e.button.clicks, mods());
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    app.mouse_up({e.button.x, e.button.y}, e.button.button, mods());
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    app.mouse_move({e.motion.x, e.motion.y}, mods());
                    break;
                case SDL_EVENT_MOUSE_WHEEL: {
                    double k = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
                    app.wheel({e.wheel.mouse_x, e.wheel.mouse_y}, e.wheel.x * k,
                              e.wheel.y * k, mods());
                    break;
                }
                case SDL_EVENT_PINCH_UPDATE:
                    app.pinch(e.pinch.scale);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    app.key_down(e.key.key, mods());
                    break;
                case SDL_EVENT_KEY_UP:
                    app.key_up(e.key.key);
                    break;
                case SDL_EVENT_TEXT_INPUT:
                    app.text_input(e.text.text);
                    break;
                case SDL_EVENT_DROP_FILE:
                    app.drop_file(e.drop.data, {e.drop.x, e.drop.y});
                    break;
                default:
                    break;
                }
            }

            int w, h, pw, ph;
            SDL_GetWindowSize(window, &w, &h);
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            app.update(SDL_GetTicks() / 1000.0, w, h);

            glViewport(0, 0, pw, ph);
            glClearColor(1, 1, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            app.render(w > 0 ? float(pw) / float(w) : 1.0f);
            SDL_GL_SwapWindow(window);
        }
    }

    nvgDeleteGL3(vg);
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}
