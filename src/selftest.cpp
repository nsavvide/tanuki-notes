// `tanuki-notes --selftest`: drives the real App with synthetic input and
// checks the logic that's easy to break: creating, nesting, reparenting,
// undo, text editing, save/load, and precision at extreme zoom depth.
//
// `tanuki-notes --make-demo <dir>`: builds a small example map.

#include "app.hpp"
#include "code.hpp"
#include "render.hpp"
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <print>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::println(stderr, "FAIL line {}: {}", __LINE__, #cond);         \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

double t_now = 0;
void tick(App& app) { app.update(t_now += 1 / 60.0, 1280, 800); }

void drag(App& app, Vec2 a, Vec2 b) {
    app.mouse_down(a, SDL_BUTTON_LEFT, 1, {});
    tick(app);
    app.mouse_move((a + b) * 0.5, {});
    app.mouse_move(b, {});
    app.mouse_up(b, SDL_BUTTON_LEFT, {});
    tick(app);
}

void click(App& app, Vec2 p, int clicks = 1) {
    app.mouse_down(p, SDL_BUTTON_LEFT, clicks, {});
    app.mouse_up(p, SDL_BUTTON_LEFT, {});
    tick(app);
}

void type(App& app, const char* s) { app.text_input(s); }

} // namespace

int run_selftest(NVGcontext* vg, SDL_Window* window) {
    fs::path dir = fs::temp_directory_path() /
                   ("tanuki-selftest-" + std::to_string(getpid()));
    fs::remove_all(dir);
    // "Terminal" that exits at once: the tests never open real nvim windows.
    setenv("TERMINAL", "true", 1);
    {
        App app(vg, window, dir);
        tick(app);

        // Create a rectangle by dragging.
        app.set_tool(Tool::Rect);
        drag(app, {200, 150}, {1000, 650});
        CHECK(app.root().children.size() == 1);
        Node* big = app.root().children.empty() ? nullptr : app.root().children[0].get();
        CHECK(big && big->kind == Kind::Rect);
        CHECK(app.tool() == Tool::Select);
        if (!big) {
            return 1;
        }
        auto r = app.screen_rect_of(big);
        CHECK(r && near(r->x, 200) && near(r->w, 800) && near(r->h, 500));

        // A shape drawn inside another one becomes its child.
        app.set_tool(Tool::Rect);
        drag(app, {400, 300}, {600, 450});
        CHECK(big->children.size() == 1);
        Node* small = big->children.empty() ? nullptr : big->children[0].get();
        if (!small) {
            return 1;
        }

        // Dragging it out of the big one reparents it, keeping its position.
        click(app, {600, 600}); // deselect
        drag(app, {400, 375}, {50, 375});
        CHECK(small->parent == &app.root());
        r = app.screen_rect_of(small);
        CHECK(r && near(r->x, 50, 1e-6) && near(r->y, 300, 1e-6) && near(r->w, 200));

        // Undo puts it back.
        app.undo();
        tick(app);
        CHECK(app.root().children.size() == 1 && app.root().children[0]->children.size() == 1);
        big = app.root().children[0].get();
        small = big->children[0].get();

        // Text: click with the text tool, type, escape.
        app.set_tool(Tool::Text);
        click(app, {300, 720});
        type(app, "hello");
        type(app, " zoom");
        app.key_down(SDLK_ESCAPE, {});
        tick(app);
        bool found = false;
        for (auto& c : app.root().children) {
            found |= c->kind == Kind::Text && c->text == "hello zoom";
        }
        CHECK(found);

        // Label a shape: double-click it, type.
        click(app, {600, 600}); // deselect
        app.mouse_down({400, 375}, SDL_BUTTON_LEFT, 2, {});
        app.mouse_up({400, 375}, SDL_BUTTON_LEFT, {});
        type(app, "Rust");
        app.key_down(SDLK_ESCAPE, {});
        CHECK(small->text == "Rust");

        // Deep nesting: 80 levels inside `small`, then zoom all the way in.
        Node* deep = small;
        for (int i = 0; i < 80; ++i) {
            auto n = make_node(Kind::Rect);
            n->x = 0.4;
            n->y = 0.4 * deep->aspect();
            n->w = 0.2;
            n->h = 0.2 * deep->aspect();
            n->style.stroke_width = 0.002;
            deep = &add_child(*deep, std::move(n));
        }
        app.selection_.clear();
        for (int i = 0; i < 4000 && depth(app.anchor()) < 82; ++i) {
            auto dr = app.screen_rect_of(deep);
            Vec2 target = dr ? dr->center() : Vec2{640, 400};
            app.wheel(target, 0, 1, {});
            tick(app);
        }
        CHECK(depth(app.anchor()) >= 80);
        std::println("deepest anchor depth: {}", depth(app.anchor()));
        auto before = app.screen_rect_of(deep);
        app.camera().pan(1, 0);
        auto after = app.screen_rect_of(deep);
        CHECK(before && after && near(after->x - before->x, 1, 1e-6));
        CHECK(before && after && near(after->w, before->w, 1e-6));

        // Zoom back out: the anchor climbs back up.
        for (int i = 0; i < 4000 && app.anchor() != &app.root(); ++i) {
            app.wheel({640, 400}, 0, -3, {});
            tick(app);
        }
        CHECK(app.anchor() == &app.root());

        // Back to the starting view for the editing checks below.
        app.anchor_ = &app.root();
        app.cam_ = Camera{0.5 - 640 / 1e6, 0.5 - 400 / 1e6, 1e6};
        tick(app);

        // Resize with the bottom-right handle; children scale along.
        click(app, {200, 400});
        CHECK(app.selection().size() == 1 && app.selection()[0] == big);
        drag(app, {1004, 654}, {1104, 704});
        r = app.screen_rect_of(big);
        CHECK(r && near(r->w, 900) && near(r->h, 550));
        auto sr = app.screen_rect_of(small);
        CHECK(sr && near(sr->w, 225));

        // Band selection picks whole shapes inside the band.
        click(app, {1200, 120});
        drag(app, {150, 110}, {1150, 790});
        CHECK(app.selection().size() == 2); // big rect + "hello zoom"

        // Duplicate.
        size_t before_dup = app.root().children.size();
        app.key_down(SDLK_D, {false, true, false});
        CHECK(app.root().children.size() == before_dup + 2);
        app.undo();
        tick(app);
        big = app.root().children[0].get();
        small = big->children[0].get();

        // Eraser removes what it touches.
        app.set_tool(Tool::Line);
        drag(app, {40, 770}, {140, 770});
        size_t with_line = app.root().children.size();
        app.set_tool(Tool::Eraser);
        drag(app, {90, 750}, {90, 790});
        CHECK(app.root().children.size() == with_line - 1);

        // Images: aspect ratio is kept, file is copied into assets/.
        fs::path img = dir.parent_path() / ("tanuki-test-" + std::to_string(getpid()) + ".png");
        SDL_Surface* surf = SDL_CreateSurface(64, 32, SDL_PIXELFORMAT_RGBA32);
        SDL_FillSurfaceRect(surf, nullptr, 0xff3366ff);
        SDL_SavePNG(surf, img.c_str());
        SDL_DestroySurface(surf);
        app.insert_file(img.string(), {640, 120});
        Node* image = app.root().children.back().get();
        CHECK(image->kind == Kind::Image && near(image->aspect(), 0.5, 1e-9));
        CHECK(fs::exists(dir / image->asset));
        fs::remove(img);

        // PDFs: one container with a child per page.
        fs::path pdf = dir.parent_path() / ("tanuki-test-" + std::to_string(getpid()) + ".pdf");
        {
            std::ofstream out(pdf);
            out << "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
                   "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
                   "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 100]>>endobj\n"
                   "trailer<</Root 1 0 R>>\n%%EOF\n";
        }
        app.insert_file(pdf.string(), {900, 120});
        Node* doc = app.root().children.back().get();
        CHECK(doc->kind == Kind::Pdf && doc->children.size() == 1);
        CHECK(!doc->children.empty() && near(doc->children[0]->aspect(), 0.5, 1e-9));
        fs::remove(pdf);

        // Search finds the "Rust" label and flies to it.
        app.selection_.clear();
        app.key_down(SDLK_F, {false, true, false});
        type(app, "rus");
        CHECK(!app.search_results_.empty() &&
              app.search_results_[0].node->text == "Rust");
        app.key_down(SDLK_RETURN, {});
        CHECK(!app.search_open_ && app.flying_);
        for (int i = 0; i < 600 && app.flying_; ++i) {
            tick(app);
        }
        auto found_r = app.screen_rect_of(small);
        CHECK(found_r && near(found_r->center().x, 640, 1) &&
              near(found_r->center().y, 400, 1));
        CHECK(app.selection().size() == 1 && app.selection()[0] == small);

        // Search also looks inside PDFs (the test page has no text, so this
        // just exercises the path) and by file name.
        app.key_down(SDLK_F, {false, true, false});
        type(app, "tanuki-test");
        CHECK(!app.search_results_.empty());
        app.key_down(SDLK_ESCAPE, {});
        CHECK(!app.search_open_);

        // Draw a few frames through every code path (smoke test).
        for (int i = 0; i < 5; ++i) {
            tick(app);
            app.render(1);
        }

        // Save and load give back the same tree.
        app.flush();
        auto loaded = store::load(dir);
        CHECK(node_to_json(*loaded, true) == node_to_json(app.root(), true));
        CHECK(fs::exists(dir / "board.json"));

        // Git: commit creates the repo; a second commit has nothing to do.
        CHECK(store::git_commit(dir) == "Committed");
        CHECK(store::git_commit(dir) == "Nothing to commit");
        std::println("map written to {}", dir.string());
    }

    // A map inside a project repo commits only itself, no nested repo.
    {
        fs::path proj = dir;
        proj += "-proj";
        fs::remove_all(proj);
        fs::create_directories(proj / "docs" / "map");
        std::string git = "git -C '" + proj.string() + "' ";
        std::ofstream(proj / "code.txt") << "draft\n";
        CHECK(std::system((git + "init -q && " + git + "add code.txt").c_str()) == 0);
        std::ofstream(proj / "docs" / "map" / "board.json") << "{}\n";
        CHECK(store::git_commit(proj / "docs" / "map") == "Committed");
        CHECK(!fs::exists(proj / "docs" / "map" / ".git"));
        // code.txt is still staged, not committed.
        CHECK(std::system((git + "diff --cached --quiet -- code.txt").c_str()) != 0);
        CHECK(std::system((git + "cat-file -e HEAD:docs/map/board.json").c_str()) == 0);
        fs::remove_all(proj);
    }

    // Double-click: blank shape -> edit label; shape with content -> dive in;
    // mouse back button -> out again.
    {
        fs::path d = dir;
        d += "-dive";
        fs::remove_all(d);
        {
            App app(vg, window, d);
            tick(app);
            app.set_tool(Tool::Rect);
            drag(app, {100, 100}, {500, 400});
            app.set_tool(Tool::Rect);
            drag(app, {900, 500}, {1000, 600});
            Node* box = app.root().children[0].get();
            click(app, {300, 250}, 2);
            CHECK(app.editing_ == box);
            type(app, "Box");
            app.key_down(SDLK_ESCAPE, {});
            click(app, {1200, 750});
            tick(app);
            app.render(1); // label drawn centered
            CHECK(box->title_k == 0);
            app.set_tool(Tool::Rect);
            drag(app, {350, 300}, {450, 380});
            CHECK(box->children.size() == 1);
            click(app, {1200, 750});

            // The label glides to the top-left instead of jumping.
            tick(app);
            app.render(1);
            CHECK(box->title_k > 0 && box->title_k < 1);
            CHECK(app.busy());
            for (int i = 0; i < 30; ++i) {
                tick(app);
                app.render(1);
            }
            CHECK(box->title_k == 1);

            auto width = [&] { return app.screen_rect_of(box)->w; };
            click(app, {150, 350}, 2); // inside, away from title and child
            CHECK(!app.editing_);
            for (int i = 0; i < 120; ++i) {
                tick(app);
            }
            CHECK(width() > 800);
            app.mouse_down({640, 400}, SDL_BUTTON_X1, 1, {});
            app.mouse_up({640, 400}, SDL_BUTTON_X1, {});
            for (int i = 0; i < 120; ++i) {
                tick(app);
            }
            CHECK(width() < 700);
        }
        fs::remove_all(d);
    }

    // Code blocks, nvim sync and vim keys.
    {
        bool block = false;
        auto toks = code::highlight("rust", "let x = \"hi\"; // note", block);
        CHECK(toks.front().kind == code::Tok::Keyword);
        CHECK(toks.back().kind == code::Tok::Comment);
        CHECK(std::ranges::any_of(toks, [](auto& t) { return t.kind == code::Tok::String; }));
        code::highlight("cpp", "int a; /* open", block);
        CHECK(block);
        CHECK(code::highlight("cpp", "still */ b", block).front().kind == code::Tok::Comment);
        CHECK(!block);

        fs::path d = dir;
        d += "-vim";
        fs::remove_all(d);
        {
            App app(vg, window, d);
            tick(app);
            app.mouse_move({640, 400}, {});
            app.key_down(SDLK_C, {});
            CHECK(app.root().children.size() == 1);
            Node* code = app.root().children[0].get();
            CHECK(code->kind == Kind::Code && code->lang == "cpp");

            // A save in nvim: the file changes, the next frame picks it up.
            fs::path f = d / "edit.cpp";
            std::ofstream(f) << "int main() {\n    return 0;\n}\n";
            app.nvim_edits_.push_back({code->id, f, fs::file_time_type::min(), nullptr});
            double w0 = code->w;
            tick(app);
            CHECK(code->text == "int main() {\n    return 0;\n}");
            CHECK(code->w >= w0);
            CHECK(std::ranges::none_of(app.nvim_edits_, // no process: session ends
                                       [&](auto& e) { return e.file == f; }));
            for (int i = 0; i < 3; ++i) {
                tick(app);
                app.render(1);
            }

            // hjkl walk between three boxes in a row.
            click(app, {1200, 750});
            for (double x : {100.0, 400.0, 700.0}) {
                app.set_tool(Tool::Rect);
                drag(app, {x, 600}, {x + 150, 700});
            }
            Node* b0 = app.root().children[1].get();
            Node* b1 = app.root().children[2].get();
            Node* b2 = app.root().children[3].get();
            app.selection_ = {b0};
            app.key_down(SDLK_L, {});
            CHECK(app.selection_.size() == 1 && app.selection_[0] == b1);
            app.key_down(SDLK_L, {});
            CHECK(app.selection_[0] == b2);
            app.key_down(SDLK_H, {});
            CHECK(app.selection_[0] == b1);
            app.key_down(SDLK_K, {}); // the code block is above
            CHECK(app.selection_[0] == code);

            // Enter dives in, Esc comes back out with the box selected.
            app.selection_ = {b1};
            app.key_down(SDLK_RETURN, {});
            for (int i = 0; i < 120; ++i) {
                tick(app);
            }
            CHECK(app.screen_rect_of(b1)->w > 800);
            app.selection_.clear();
            app.key_down(SDLK_ESCAPE, {});
            CHECK(app.selection_.size() == 1 && app.selection_[0] == b1);
        }
        fs::remove_all(d);
    }

    // Arrows attached to shapes follow them.
    fs::path dir2 = dir;
    dir2 += "-bind";
    fs::remove_all(dir2);
    {
        App app(vg, window, dir2);
        tick(app);
        app.set_tool(Tool::Rect);
        drag(app, {100, 100}, {300, 250});
        app.set_tool(Tool::Ellipse);
        drag(app, {700, 100}, {900, 250});
        Node* a = app.root().children[0].get();
        Node* b = app.root().children[1].get();
        app.set_tool(Tool::Arrow);
        drag(app, {250, 175}, {800, 175});
        Node* arrow = app.root().children[2].get();
        CHECK(arrow->kind == Kind::Arrow);
        CHECK(arrow->start_bind == a->id && arrow->end_bind == b->id);
        tick(app);
        auto pts = screen_points(*arrow, *app.xform_of(arrow));
        CHECK(pts.front().x > 300 && pts.front().x < 320); // just right of a
        CHECK(pts.back().x > 680 && pts.back().x < 700);   // just left of b

        // Move b down: the arrow end follows onto b's outline.
        click(app, {1200, 700});
        drag(app, {800, 100}, {800, 400});
        tick(app);
        pts = screen_points(*arrow, *app.xform_of(arrow));
        auto br = app.screen_rect_of(b);
        CHECK(br && near(br->y, 400));
        CHECK(pts.back().y > 400 && pts.back().y < br->center().y);

        // Survives save/load.
        app.flush();
        auto loaded = store::load(dir2);
        CHECK(loaded->children.size() == 3 &&
              loaded->children[2]->end_bind == b->id);

        // Moving the arrow on its own detaches it.
        click(app, {1200, 700});
        Vec2 mid = (pts.front() + pts.back()) * 0.5;
        drag(app, mid, mid + Vec2{0, 60});
        CHECK(arrow->start_bind.empty() && arrow->end_bind.empty());
    }
    fs::remove_all(dir2);

    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("selftest: all checks passed");
    fs::remove_all(dir);
    return 0;
}

void make_demo(App& app) {
    auto tick_ = [&] { tick(app); };
    tick_();
    auto shape = [&](Tool t, Vec2 a, Vec2 b) {
        app.set_tool(t);
        drag(app, a, b);
    };
    auto label = [&](Vec2 p, const char* s) {
        app.mouse_down(p, SDL_BUTTON_LEFT, 2, {});
        app.mouse_up(p, SDL_BUTTON_LEFT, {});
        type(app, s);
        app.key_down(SDLK_ESCAPE, {});
        click(app, {1250, 780});
    };
    auto note = [&](Vec2 p, const char* s) {
        app.set_tool(Tool::Text);
        click(app, p);
        type(app, s);
        app.key_down(SDLK_ESCAPE, {});
    };

    note({90, 120}, "My knowledge");
    shape(Tool::Rect, {120, 200}, {560, 520});
    label({120, 360}, "Rust");
    shape(Tool::Ellipse, {700, 220}, {1080, 480});
    label({890, 220}, "Distributed systems");
    shape(Tool::Arrow, {570, 360}, {690, 350});
    shape(Tool::Diamond, {760, 560}, {1020, 720});
    label({760, 640}, "Sharding?");
    note({140, 600}, "Scroll into a shape\nto see what's inside");

    // Inside "Rust": a few smaller concepts.
    shape(Tool::Rect, {160, 260}, {320, 340});
    label({160, 300}, "Ownership");
    shape(Tool::Rect, {360, 260}, {520, 340});
    label({360, 300}, "Lifetimes");
    shape(Tool::Draw, {200, 420}, {480, 470});
}
