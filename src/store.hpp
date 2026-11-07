#pragma once

#include "geom.hpp"
#include "node.hpp"

#include <filesystem>
#include <memory>
#include <string>

// On-disk layout of a map folder (git-friendly: plain text, stable ids):
//
//   <map>/board.json             children of the root
//   <map>/<name>-<id>/board.json children of that node (only if it has any)
//   <map>/assets/                 copied images and PDFs
//   <map>/.view.json              last camera position (gitignored)
namespace store {

std::unique_ptr<Node> load(const std::filesystem::path& dir);

// Writes only files whose content changed; removes folders of deleted nodes.
void save(const Node& root, const std::filesystem::path& dir);

struct View {
    std::string anchor_id = "root";
    Camera camera;
    bool valid = false;
};
View load_view(const std::filesystem::path& dir);
void save_view(const std::filesystem::path& dir, const View& view);

// `git add -A && git commit` in the map folder (runs `git init` first if
// needed). Returns a short status message for the UI.
std::string git_commit(const std::filesystem::path& dir);

} // namespace store
