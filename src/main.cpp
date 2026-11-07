#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_video.h>
#include <cmath>

struct Camera {
    double x = 0, y = 0, scale = 1; // top left
    double to_screen_x(double world_x) { return (world_x - x) * scale; }
    double to_screen_y(double world_y) { return (world_y - y) * scale; }
    double to_world_x(double screen_x) { return screen_x / scale + x; }
    double to_world_y(double screen_y) { return screen_y / scale + y; }
};

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return 1;
    }

    SDL_Window* window =
        SDL_CreateWindow("tanuki-notes", 1280, 800, SDL_WINDOW_RESIZABLE);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    SDL_SetRenderVSync(renderer, 1);

    Camera camera;
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

                camera.scale *= std::pow(1.15, event.wheel.y);
                camera.x = world_x - mouse_x / camera.scale;
                camera.y = world_y - mouse_y / camera.scale;

                break;
            }
            }
        }

        SDL_SetRenderDrawColor(renderer, 250, 250, 250, 255);
        SDL_RenderClear(renderer);

        SDL_FRect rectangle{
            float(camera.to_screen_x(0)), float(camera.to_screen_y(0)),
            float(400 * camera.scale), float(300 * camera.scale)};

        SDL_SetRenderDrawColor(renderer, 0, 52, 60, 255);
        SDL_RenderFillRect(renderer, &rectangle);

        SDL_RenderPresent(renderer);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
