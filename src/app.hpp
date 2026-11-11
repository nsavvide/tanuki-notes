#pragma once

#include "assets.hpp"
#include "geom.hpp"
#include "node.hpp"
#include "ui.hpp"
#include "vim.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct NVGcontext;

enum class Tool {
    Hand, Select, Rect, Diamond, Ellipse, Arrow, Line, Draw, Text, Image, Eraser,
};

struct Mods {
    bool shift = false, ctrl = false, alt = false;
};

class App {
  public:
    App(NVGcontext* vg, SDL_Window* window, std::filesystem::path map_dir);
    ~App();

    // Input (screen coordinates in window points).
    void mouse_down(Vec2 p, int button, int clicks, Mods m);
    void mouse_move(Vec2 p, Mods m);
    void mouse_up(Vec2 p, int button, Mods m);
    void wheel(Vec2 p, double dx, double dy, Mods m);
    void pinch(double factor);
    void key_down(SDL_Keycode key, Mods m);
    void key_up(SDL_Keycode key);
    void text_input(const char* utf8);
    void drop_file(const std::string& path, Vec2 p);

    void update(double now, int win_w, int win_h);
    void render(float pixel_ratio);
    void flush(); // save to disk now

    // True while something animates or waits (so the loop keeps drawing).
    bool busy() const {
        return flying_ || editing_ || search_open_ || labels_moving_ ||
               !pending_link_.empty() || drag_ != Drag::None || dirty_ ||
               assets_.busy() || now_ < toast_until_;
    }

    // Inspection (self-test).
    Node& root() { return *root_; }
    Node* anchor() { return anchor_; }
    Camera& camera() { return cam_; }
    const std::vector<Node*>& selection() const { return selection_; }
    Tool tool() const { return tool_; }
    void set_tool(Tool t);
    void undo();
    void redo();
    void insert_file(const std::string& path, Vec2 center);

  private:
    friend int run_selftest(NVGcontext* vg, SDL_Window* window);

    struct Hit {
        Rect r;
        std::function<void()> action;
    };
    enum class Drag { None, Pan, Create, Move, Resize, Band, Erase, Endpoint };

    struct SearchResult {
        Node* node;
        std::string title, detail;
        int score;
        int depth;
    };

    // Geometry helpers.
    std::pair<Node*, Xform> draw_root() const;
    std::optional<Xform> xform_of(const Node* n) const;
    std::optional<Rect> screen_rect_of(const Node* n) const;
    Node* pick(Vec2 p);
    Node* pick_in(Node& n, Xform t, Vec2 p);
    bool hit_self(const Node& n, Xform t, Vec2 p) const;
    bool on_label(const Node& n, Vec2 p) const; // over the shape's label text
    Node* container_at(Vec2 p, const Node* exclude);
    void reroot();
    void clamp_zoom();
    void reparent(Node& n, Node& new_parent);
    void set_linear_points(Node& n, const std::vector<Vec2>& local_pts);
    std::vector<Vec2> linear_local_points(const Node& n) const;

    // Arrows and lines attached to shapes.
    Node* bind_target_at(Vec2 p, const Node* exclude);
    void bind_endpoints(Node& n);
    std::vector<Vec2> bend_handles(const Node& n, Xform t) const;
    void update_bindings();

    void edit_selected();
    void refit_editing();
    // The note line being typed in shows its Markdown; -1 when not a note.
    int raw_line() const;
    void split_fences(Node& n); // ```lang ... ``` in finished text -> code blocks

    // [[links]] between notes, by title.
    std::optional<std::string> link_at(Vec2 p);
    void follow_link(const std::string& title);
    // While typing "[[foo": where the "[[" starts, and "foo".
    std::optional<std::pair<size_t, std::string>> link_query() const;
    std::vector<std::string> link_matches(const std::string& query) const;
    void accept_link();
    void draw_link_popup();
    void open_code_block(const std::string& tag); // typed ```tag + Enter in a note
    void close_code_block();                      // typed ``` + Enter in a code block

    // Search.
    void open_search();
    void close_search();
    void run_search();
    void search_go(int index);
    void draw_search();

    // Editing.
    void start_create(Vec2 p, Mods m);
    void update_create(Vec2 p, Mods m);
    void finish_create();
    void start_text_edit(Node& n, bool select_end = true);
    void finish_text_edit();
    void delete_selection();
    void duplicate_selection();
    void copy_selection();
    void paste(Vec2 at);
    void select_all();
    void move_layer(bool to_front);
    void nudge(double dx, double dy);
    std::vector<Node*> top_level_selection() const;
    void apply_to_selection(const std::function<void(Node&)>& f);
    void place_new_node(Node& parent, Node& n, Rect screen);

    // History and persistence.
    void commit();
    void restore(const std::string& state);
    void mark_dirty();
    void toast(const std::string& msg);
    void open_file_dialog();

    // Navigation.
    void fly_to(const Node* n);
    void fly_to_content();
    void fly_out();
    void step_fly(double dt);

    // UI.
    void draw_ui();
    void draw_overlay();
    bool ui_click(Vec2 p);
    void add_hit(Rect r, std::function<void()> action);
    bool button(Rect r, ui::Icon icon, bool active,
                std::function<void()> action, const char* hint = nullptr);
    bool style_panel_visible() const;
    void draw_style_panel();

    NVGcontext* vg_;
    SDL_Window* window_;
    std::filesystem::path dir_;
    Assets assets_;

    std::unique_ptr<Node> root_;
    Node* anchor_ = nullptr;
    Camera cam_;
    int win_w_ = 1280, win_h_ = 800;
    float pixel_ratio_ = 1;
    double now_ = 0, last_frame_ = 0, frame_dt_ = 1 / 60.0;
    bool labels_moving_ = false;
    Vec2 mouse_;

    Tool tool_ = Tool::Select;
    bool space_down_ = false;
    std::vector<Node*> selection_;

    // Current style for new shapes (pixel sizes at the zoom of creation).
    Style style_;
    double stroke_px_ = 2;
    double font_px_ = 20;

    // Drag state.
    Drag drag_ = Drag::None;
    Vec2 drag_start_, drag_last_;
    Node* creating_ = nullptr;
    Vec2 create_start_local_;
    std::vector<Vec2> draw_local_;
    int resize_handle_ = -1;
    Rect resize_orig_screen_;
    double resize_orig_font_ = 0;
    std::set<Node*> erasing_;
    bool moved_ = false;

    // Text editing.
    Node* editing_ = nullptr;
    size_t caret_ = 0;
    vim::Editor vim_;
    int link_sel_ = 0;
    std::string link_last_query_;
    std::string pending_link_; // clicked link, followed unless it's a double-click
    double pending_link_at_ = 0;
    double caret_blink_start_ = 0;

    // History.
    std::vector<std::string> history_;
    int history_pos_ = -1;

    // Clipboard (internal copy/paste of nodes).
    std::string clipboard_;
    std::string clipboard_text_;

    // Persistence.
    bool dirty_ = false;
    double last_change_ = 0;
    std::string save_status_ = "Saved";

    // Fly-to animation target.
    std::string fly_id_;
    bool fly_content_ = false;
    bool flying_ = false;

    std::string came_from_id_; // shape selected by zooming out of it
    std::string code_lang_ = "cpp";
    bool auto_color_ = true; // new shapes get their own color

    // Gives a shape a light fill and dark outline in a hue its siblings and
    // parent don't use yet.
    void auto_color(Node& n);

    bool search_open_ = false;
    std::string search_query_;
    std::vector<SearchResult> search_results_;
    int search_sel_ = 0;
    Rect search_panel_;

    std::string toast_;
    double toast_until_ = 0;

    std::vector<Hit> hits_, next_hits_;

    std::mutex pending_mutex_;
    std::vector<std::string> pending_files_;
};
