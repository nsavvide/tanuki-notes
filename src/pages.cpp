#include "pages.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace pages {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) {
        s.replace(p, from.size(), to);
    }
    return s;
}

std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') {
            l.pop_back();
        }
        out.push_back(l);
    }
    return out;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Longest first, so "-->" isn't read as "--".
const char* const ops[] = {"<->", "-->", "..>", "->", "--", ".."};

// "[[a]] rest" at `s`: a, and where rest starts. npos if not a [[ref]].
size_t ref_at(const std::string& s, size_t at, std::string& ref) {
    if (s.compare(at, 2, "[[") != 0) {
        return std::string::npos;
    }
    size_t end = s.find("]]", at + 2);
    if (end == std::string::npos || end == at + 2) {
        return std::string::npos;
    }
    ref = s.substr(at + 2, end - at - 2);
    return end + 2;
}

// ": label" or nothing after a ref; false if there's other text.
bool label_after(const std::string& s, size_t at, std::string& label) {
    std::string rest = trim(s.substr(at));
    if (rest.empty()) {
        return true;
    }
    if (rest[0] != ':') {
        return false;
    }
    label = replace_all(trim(rest.substr(1)), "<br>", "\n");
    return true;
}

bool parse_link(const std::string& line, Link& out) {
    for (const char* op : ops) {
        std::string o = op;
        if (line.compare(0, o.size() + 1, o + " ") != 0) {
            continue;
        }
        size_t p = line.find_first_not_of(' ', o.size());
        std::string ref, label;
        size_t after = p == std::string::npos ? p : ref_at(line, p, ref);
        if (after == std::string::npos || !label_after(line, after, label)) {
            return false;
        }
        out = {o, ref, label};
        return true;
    }
    return false;
}

bool parse_step(const std::string& line, Step& out) {
    std::string from, to, label;
    size_t p = ref_at(line, 0, from);
    if (p == std::string::npos) {
        return false;
    }
    p = line.find_first_not_of(' ', p);
    for (const char* op : ops) {
        std::string o = op;
        if (p == std::string::npos || line.compare(p, o.size() + 1, o + " ") != 0) {
            continue;
        }
        size_t q = line.find_first_not_of(' ', p + o.size());
        size_t after = q == std::string::npos ? q : ref_at(line, q, to);
        if (after == std::string::npos || !label_after(line, after, label)) {
            return false;
        }
        out = {from, o, to, label};
        return true;
    }
    return false;
}

// "## Title" -> 2, "Title".
int heading(const std::string& line, std::string& title) {
    int n = 0;
    while (n < int(line.size()) && line[size_t(n)] == '#') {
        ++n;
    }
    if (n == 0 || n > 6 || line.size() <= size_t(n) || line[size_t(n)] != ' ') {
        return 0;
    }
    title = replace_all(trim(line.substr(size_t(n))), "<br>", "\n");
    return n;
}

bool is_fence(const std::string& line) { return trim(line).starts_with("```"); }

// Note lines that would read as page structure get a leading backslash.
bool needs_escape(const std::string& line) {
    std::string t = trim(line), title;
    Link link;
    return heading(t, title) || t.starts_with("```") || t.starts_with("\\") ||
           t.starts_with("issue:") || t == "---" || parse_link(t, link);
}

void trim_blank_lines(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
    }
    size_t start = 0;
    while (true) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos || !trim(s.substr(start, nl - start)).empty()) {
            break;
        }
        start = nl + 1;
    }
    s.erase(0, start);
}

// Notes, issues and arrows of one box from its body lines.
void fill(Box& box, const std::vector<std::string>& body) {
    std::string text, code, lang;
    bool in_code = false;
    auto flush_text = [&] {
        trim_blank_lines(text);
        if (!text.empty()) {
            box.notes.push_back({false, text, ""});
        }
        text.clear();
    };
    for (const std::string& line : body) {
        if (in_code) {
            if (is_fence(line)) {
                if (!code.empty()) {
                    code.pop_back();
                }
                box.notes.push_back({true, code, lang});
                in_code = false;
            } else {
                code += line + "\n";
            }
            continue;
        }
        if (!line.empty() && line[0] == '\\') {
            text += line.substr(1) + "\n";
            continue;
        }
        std::string t = trim(line);
        Link link;
        if (t.starts_with("```")) {
            flush_text();
            in_code = true;
            lang = trim(t.substr(3));
            code.clear();
        } else if (t.starts_with("issue:")) {
            if (!trim(t.substr(6)).empty()) {
                box.issues.push_back(trim(t.substr(6)));
            }
        } else if (parse_link(t, link)) {
            box.links.push_back(link);
        } else if (t == "---") {
            flush_text();
        } else {
            text += line + "\n";
        }
    }
    if (in_code) {
        box.notes.push_back({true, code, lang});
    }
    flush_text();
}

std::string encode(const std::string& s) { return replace_all(s, "\n", "<br>"); }

// The body of a box (issues, notes, arrows) as blank-line separated blocks.
std::vector<std::string> body_blocks(const Box& b) {
    std::vector<std::string> blocks;
    if (!b.issues.empty()) {
        std::string s;
        for (const auto& i : b.issues) {
            s += (s.empty() ? "" : "\n") + std::string("issue: ") + i;
        }
        blocks.push_back(s);
    }
    for (size_t i = 0; i < b.notes.size(); ++i) {
        const Note& n = b.notes[i];
        if (i > 0 && !n.code && !b.notes[i - 1].code) {
            blocks.push_back("---"); // two notes in a row
        }
        if (n.code) {
            blocks.push_back("```" + n.lang + "\n" + n.text + "\n```");
            continue;
        }
        std::string s;
        for (const auto& l : lines_of(n.text)) {
            s += (s.empty() ? "" : "\n") + (needs_escape(l) ? "\\" + l : l);
        }
        blocks.push_back(s);
    }
    if (!b.links.empty()) {
        std::string s;
        for (const auto& l : b.links) {
            s += (s.empty() ? "" : "\n") + l.op + " [[" + l.target + "]]" +
                 (l.label.empty() ? "" : ": " + encode(l.label));
        }
        blocks.push_back(s);
    }
    return blocks;
}

std::string join(const std::vector<std::string>& blocks) {
    std::string s;
    for (const auto& b : blocks) {
        s += (s.empty() ? "" : "\n\n") + b;
    }
    return s.empty() ? s : s + "\n";
}

} // namespace

std::string slug(const std::string& text) {
    std::string out;
    for (char c : text.substr(0, text.find('\n'))) {
        if (std::isalnum((unsigned char)c)) {
            out += char(std::tolower((unsigned char)c));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
        if (out.size() >= 40) {
            break;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? "box" : out;
}

void parse_page(const std::string& text, Box& box) {
    struct Sec {
        int level;
        std::string title;
        std::vector<std::string> body;
    };
    std::vector<Sec> secs{{1, "", {}}};
    bool in_code = false, titled = false;
    for (const std::string& line : lines_of(text)) {
        std::string title;
        int lvl = in_code ? 0 : heading(line, title);
        if (lvl == 1 && !titled && secs.size() == 1 &&
            std::all_of(secs[0].body.begin(), secs[0].body.end(),
                        [](auto& l) { return trim(l).empty(); })) {
            box.title = title;
            titled = true;
            continue;
        }
        if (lvl >= 1) {
            secs.push_back({std::max(lvl, 2), title, {}});
            continue;
        }
        if (is_fence(line) && !(line[0] == '\\')) {
            in_code = !in_code;
        }
        secs.back().body.push_back(line);
    }
    fill(box, secs[0].body);
    size_t i = 1;
    std::function<void(Box&, int)> kids = [&](Box& parent, int level) {
        while (i < secs.size() && secs[i].level > level) {
            const Sec& s = secs[i++];
            Box k;
            k.title = s.title;
            fill(k, s.body);
            kids(k, s.level);
            parent.kids.push_back(std::move(k));
        }
    };
    kids(box, 1);
}

std::map<std::string, std::string> render(const Box& root) {
    std::map<std::string, std::string> files;
    // A box's page text from heading `level`; kids with their own file are
    // rendered as files of their own.
    std::function<std::vector<std::string>(const Box&, int)> section;
    std::function<void(const Box&)> page = [&](const Box& b) {
        auto blocks = section(b, 1);
        if (b.file == "_index.md" && !blocks.empty()) {
            blocks.erase(blocks.begin()); // the top level has no title
        }
        if (b.file != "_index.md" || !blocks.empty()) {
            files[b.file] = join(blocks);
        }
    };
    section = [&](const Box& b, int level) {
        std::vector<std::string> blocks{std::string(size_t(std::min(level, 6)), '#') + " " +
                                        encode(b.title)};
        for (auto& bl : body_blocks(b)) {
            blocks.push_back(bl);
        }
        for (const Box& k : b.kids) {
            if (!k.file.empty()) {
                page(k);
                continue;
            }
            for (auto& bl : section(k, level + 1)) {
                blocks.push_back(bl);
            }
        }
        return blocks;
    };
    page(root);
    return files;
}

std::map<std::string, fs::file_time_type> stamps(const fs::path& dir) {
    std::map<std::string, fs::file_time_type> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (it->is_directory() && (name.starts_with(".") ||
                                   (name == "assets" && it->path().parent_path() == dir))) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() && it->path().extension() == ".md") {
            out[fs::relative(it->path(), dir).generic_string()] = it->last_write_time(ec);
        }
    }
    return out;
}

Map read(const fs::path& dir) {
    Map m;
    std::function<void(Box&, const fs::path&, const std::string&)> folder =
        [&](Box& b, const fs::path& path, const std::string& rel) {
            std::vector<fs::directory_entry> entries;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(path, ec)) {
                entries.push_back(e);
            }
            std::sort(entries.begin(), entries.end(),
                      [](auto& a, auto& c) { return a.path().filename() < c.path().filename(); });
            for (const auto& e : entries) {
                std::string name = e.path().filename().string();
                if (name.starts_with(".") || (rel.empty() && (name == "assets" || name == "flows"))) {
                    continue;
                }
                if (e.is_directory() && fs::exists(e.path() / "_index.md")) {
                    Box k;
                    k.file = rel + name + "/_index.md";
                    parse_page(read_file(e.path() / "_index.md"), k);
                    if (k.title.empty()) {
                        k.title = name;
                    }
                    folder(k, e.path(), rel + name + "/");
                    b.kids.push_back(std::move(k));
                    m.any = true;
                } else if (e.is_regular_file() && e.path().extension() == ".md" &&
                           name != "_index.md") {
                    Box k;
                    k.file = rel + name;
                    parse_page(read_file(e.path()), k);
                    if (k.title.empty()) {
                        k.title = e.path().stem().string();
                    }
                    b.kids.push_back(std::move(k));
                    m.any = true;
                }
            }
        };
    m.root.file = "_index.md";
    if (fs::exists(dir / "_index.md")) {
        parse_page(read_file(dir / "_index.md"), m.root);
        m.any = true;
    }
    m.root.title.clear();
    folder(m.root, dir, "");

    std::error_code ec;
    std::vector<fs::path> flow_files;
    for (const auto& e : fs::directory_iterator(dir / "flows", ec)) {
        if (e.path().extension() == ".md") {
            flow_files.push_back(e.path());
        }
    }
    std::sort(flow_files.begin(), flow_files.end());
    for (const auto& p : flow_files) {
        Flow f;
        f.id = p.stem().string();
        f.title = f.id;
        std::string note;
        bool titled = false;
        for (const auto& line : lines_of(read_file(p))) {
            std::string title;
            Step step;
            if (!titled && heading(line, title) == 1) {
                f.title = title;
                titled = true;
            } else if (parse_step(trim(line), step)) {
                f.steps.push_back(step);
            } else {
                note += line + "\n";
            }
        }
        trim_blank_lines(note);
        f.note = note;
        m.flows.push_back(std::move(f));
    }
    return m;
}

void write(const fs::path& dir, const std::map<std::string, std::string>& files,
           const std::set<std::string>& deletable) {
    for (const auto& [rel, content] : files) {
        fs::path p = dir / rel;
        if (fs::exists(p) && read_file(p) == content) {
            continue;
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
    for (const auto& rel : deletable) {
        if (!files.contains(rel) && !rel.starts_with("flows/")) {
            std::error_code ec;
            fs::remove(dir / rel, ec);
        }
    }
}

std::string hash_id(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h = (h ^ c) * 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%012llx", (unsigned long long)(h & 0xffffffffffffull));
    return buf;
}

std::string issue_url(const std::string& issue) { return issue.substr(0, issue.find(' ')); }

std::string issue_title(const std::string& issue) {
    size_t sp = issue.find(' ');
    if (sp != std::string::npos && !trim(issue.substr(sp)).empty()) {
        return trim(issue.substr(sp));
    }
    // https://www.notion.so/team/Order-import-retries-1a2b...(32 hex) -> "Order import retries"
    std::string u = issue.substr(0, issue.find_first_of("?#"));
    while (!u.empty() && u.back() == '/') {
        u.pop_back();
    }
    std::string last = u.substr(u.find_last_of('/') + 1);
    size_t dash = last.find_last_of('-');
    std::string tail = dash == std::string::npos ? last : last.substr(dash + 1);
    if (tail.size() == 32 &&
        std::all_of(tail.begin(), tail.end(), [](char c) { return std::isxdigit((unsigned char)c); })) {
        last = dash == std::string::npos ? "" : last.substr(0, dash);
    }
    std::replace(last.begin(), last.end(), '-', ' ');
    return last.empty() ? u : last;
}

} // namespace pages
