#include "store.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace store {

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Atomic write, skipped when the content is identical (keeps git quiet).
static void write_if_changed(const fs::path& p, const std::string& content) {
    if (fs::exists(p) && read_file(p) == content) {
        return;
    }
    fs::create_directories(p.parent_path());
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << content;
    }
    fs::rename(tmp, p);
}

static std::string slug(const std::string& text) {
    std::string out;
    for (char c : text.substr(0, text.find('\n'))) {
        if (std::isalnum((unsigned char)c)) {
            out += char(std::tolower((unsigned char)c));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
        if (out.size() >= 32) {
            break;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

static std::string folder_name(const Node& n) {
    std::string s = slug(n.text);
    return (s.empty() ? std::string(kind_name(n.kind)) : s) + "-" +
           n.id.substr(0, 6);
}

static void collect(const Node& node, const fs::path& folder,
                    std::vector<std::pair<fs::path, std::string>>& files,
                    std::set<fs::path>& folders) {
    folders.insert(folder);
    json nodes = json::array();
    for (const auto& c : node.children) {
        json j = node_to_json(*c, false);
        if (!c->children.empty()) {
            j["folder"] = folder_name(*c);
            collect(*c, folder / folder_name(*c), files, folders);
        }
        nodes.push_back(j);
    }
    json board = {{"tanuki", 1}, {"nodes", nodes}};
    files.emplace_back(folder / "board.json", board.dump(2) + "\n");
}

void save(const Node& root, const fs::path& dir) {
    std::vector<std::pair<fs::path, std::string>> files;
    std::set<fs::path> folders;
    collect(root, dir, files, folders);
    for (const auto& [path, content] : files) {
        write_if_changed(path, content);
    }
    write_if_changed(dir / ".gitignore", ".view.json\n*.tmp\n");

    // Remove board folders of nodes that no longer exist. Only touches
    // folders that contain a board.json, never assets or .git.
    std::vector<fs::path> stale;
    for (auto it = fs::recursive_directory_iterator(dir);
         it != fs::recursive_directory_iterator(); ++it) {
        const fs::path& p = it->path();
        if (it->is_directory() && p.filename().string().starts_with(".")) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory() && p.filename() == "assets" &&
            p.parent_path() == dir) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory() && !folders.contains(p) &&
            fs::exists(p / "board.json")) {
            stale.push_back(p);
            it.disable_recursion_pending();
        }
    }
    for (const auto& p : stale) {
        fs::remove_all(p);
    }
}

static void load_children(Node& node, const fs::path& folder) {
    fs::path board = folder / "board.json";
    if (!fs::exists(board)) {
        return;
    }
    json j = json::parse(read_file(board), nullptr, false);
    if (j.is_discarded() || !j.contains("nodes")) {
        return;
    }
    for (const auto& nj : j["nodes"]) {
        Node& child = add_child(node, node_from_json(nj));
        if (nj.contains("folder")) {
            load_children(child, folder / nj["folder"].get<std::string>());
        }
    }
}

std::unique_ptr<Node> load(const fs::path& dir) {
    auto root = make_root();
    load_children(*root, dir);
    return root;
}

View load_view(const fs::path& dir) {
    View v;
    json j = json::parse(read_file(dir / ".view.json"), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return v;
    }
    v.anchor_id = j.value("anchor", "root");
    v.camera = {j.value("x", 0.0), j.value("y", 0.0), j.value("scale", 1.0)};
    v.valid = true;
    return v;
}

void save_view(const fs::path& dir, const View& v) {
    json j = {{"anchor", v.anchor_id},
              {"x", v.camera.x},
              {"y", v.camera.y},
              {"scale", v.camera.scale}};
    write_if_changed(dir / ".view.json", j.dump(2) + "\n");
}

static int run(const std::string& cmd) { return std::system(cmd.c_str()); }

std::string git_commit(const fs::path& dir) {
    std::string q = "'" + dir.string() + "'";
    if (!fs::exists(dir / ".git") && run("git -C " + q + " init -q") != 0) {
        return "git init failed";
    }
    run("git -C " + q + " add -A");
    if (run("git -C " + q + " diff --cached --quiet") == 0) {
        return "Nothing to commit";
    }
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", std::localtime(&t));
    std::string msg = std::string("tanuki: ") + stamp;
    if (run("git -C " + q + " commit -q -m '" + msg + "'") != 0) {
        return "git commit failed";
    }
    return "Committed";
}

} // namespace store
