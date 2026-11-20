#pragma once

// The map as Markdown pages (pure text, no app state).
//
//   <map>/_index.md            the top level: notes, `##` boxes, arrows
//   <map>/qargo/_index.md      a box with its own folder (`# Qargo`)
//   <map>/qargo/hasura.md      a box with its own page but no folder
//   <map>/flows/*.md           flows: numbered arrows across the map
//
// In a page, `#` is the box's title, `##`/`###`… are boxes inside it, text is
// notes (split by `---`, ``` fences are code cards), `-> [[Box]]: label` is an
// arrow from this box and `issue: <url> [title]` links an issue. Note lines
// that would read as structure are escaped with a leading `\`.

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pages {

struct Note {
    bool code = false;
    std::string text, lang;
    bool operator==(const Note&) const = default;
};

// op: -> --> ..> <-> -- ..
struct Link {
    std::string op, target, label;
    bool operator==(const Link&) const = default;
};

struct Box {
    std::string title; // may hold "\n" (written as <br>)
    std::string file;  // map-relative page file, or "" for a `##` section
    std::vector<std::string> issues; // "url" or "url title"
    std::vector<Note> notes;
    std::vector<Link> links;
    std::vector<Box> kids;
    bool operator==(const Box&) const = default;
};

struct Step {
    std::string from, op, to, label;
};

struct Flow {
    std::string id, title, note; // id: file name without .md
    std::vector<Step> steps;
};

struct Map {
    Box root;
    std::vector<Flow> flows;
    bool any = false; // the folder has pages at all
};

// Reads every page under `dir` (skips hidden folders and assets/).
Map read(const std::filesystem::path& dir);

// Parses one page's text into `box` (title, notes, links, `##` kids).
void parse_page(const std::string& text, Box& box);

// Page files for a tree: map-relative path -> content.
std::map<std::string, std::string> render(const Box& root);

// Writes changed files, and deletes the `deletable` pages (ones we read or
// wrote before) that are no longer in `files`. Never anything else: a stray
// README.md in the folder is left alone.
void write(const std::filesystem::path& dir, const std::map<std::string, std::string>& files,
           const std::set<std::string>& deletable);

// Every page file under `dir` with its modification time, to notice edits.
std::map<std::string, std::filesystem::file_time_type> stamps(const std::filesystem::path& dir);

// A title for an issue line: its own title, or one made from the URL.
std::string issue_title(const std::string& issue);
std::string issue_url(const std::string& issue);

std::string slug(const std::string& s);

// Stable 12-hex-digit id for a string.
std::string hash_id(const std::string& s);

} // namespace pages
