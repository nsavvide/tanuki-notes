#include "vim.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace vim {

namespace {

size_t next(const std::string& s, size_t i) {
    if (i >= s.size()) {
        return s.size();
    }
    ++i;
    while (i < s.size() && (s[i] & 0xc0) == 0x80) {
        ++i;
    }
    return i;
}

size_t prev(const std::string& s, size_t i) {
    if (i == 0) {
        return 0;
    }
    --i;
    while (i > 0 && (s[i] & 0xc0) == 0x80) {
        --i;
    }
    return i;
}

size_t line_begin(const std::string& s, size_t i) {
    if (i == 0) {
        return 0;
    }
    size_t nl = s.rfind('\n', i - 1);
    return nl == std::string::npos ? 0 : nl + 1;
}

size_t line_end(const std::string& s, size_t i) {
    size_t nl = s.find('\n', i);
    return nl == std::string::npos ? s.size() : nl;
}

size_t first_nonblank(const std::string& s, size_t i) {
    size_t p = line_begin(s, i), e = line_end(s, i);
    while (p < e && (s[p] == ' ' || s[p] == '\t')) {
        ++p;
    }
    return p;
}

// 0 blank, 1 word character, 2 punctuation.
int cls(const std::string& s, size_t i) {
    if (i >= s.size()) {
        return 0;
    }
    unsigned char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n') {
        return 0;
    }
    return std::isalnum(c) || c == '_' || c >= 0x80 ? 1 : 2;
}

size_t word_next(const std::string& s, size_t i) {
    int c = cls(s, i);
    if (c != 0) {
        while (i < s.size() && cls(s, i) == c) {
            ++i;
        }
    }
    while (i < s.size() && cls(s, i) == 0) {
        ++i;
    }
    return i;
}

size_t word_end(const std::string& s, size_t i) {
    i = next(s, i);
    while (i < s.size() && cls(s, i) == 0) {
        ++i;
    }
    int c = cls(s, i);
    while (i + 1 < s.size() && cls(s, i + 1) == c && c != 0) {
        ++i;
    }
    while (i > 0 && (s[i] & 0xc0) == 0x80) {
        --i;
    }
    return std::min(i, s.empty() ? 0 : s.size() - 1);
}

size_t word_prev(const std::string& s, size_t i) {
    while (i > 0 && cls(s, i - 1) == 0) {
        --i;
    }
    if (i == 0) {
        return 0;
    }
    int c = cls(s, i - 1);
    while (i > 0 && cls(s, i - 1) == c) {
        --i;
    }
    return i;
}

// Keeps the Normal-mode caret on a character (not past the line's end).
size_t clamp(const std::string& s, size_t c) {
    c = std::min(c, s.size());
    while (c > 0 && c < s.size() && (s[c] & 0xc0) == 0x80) {
        --c;
    }
    size_t lb = line_begin(s, c), le = line_end(s, c);
    return c >= le && le > lb ? prev(s, le) : c;
}

struct Motion {
    size_t pos = 0;
    bool linewise = false, inclusive = false, ok = true;
};

bool is_motion(char m) { return std::strchr("hjklwbe0^$GgfFtT", m) != nullptr; }

Motion motion(const std::string& s, size_t c, char m, std::string_view arg,
              int count, bool counted, bool op) {
    Motion r{c};
    size_t lb = line_begin(s, c), le = line_end(s, c);
    switch (m) {
    case 'h':
        for (int i = 0; i < count && r.pos > lb; ++i) {
            r.pos = prev(s, r.pos);
        }
        break;
    case 'l':
        for (int i = 0; i < count && r.pos < le; ++i) {
            r.pos = next(s, r.pos);
        }
        break;
    case 'j':
    case 'k': {
        r.linewise = true;
        size_t col = c - lb, line = lb;
        for (int i = 0; i < count; ++i) {
            if (m == 'j') {
                size_t e = line_end(s, line);
                if (e >= s.size()) {
                    r.ok = i > 0;
                    break;
                }
                line = e + 1;
            } else {
                if (line == 0) {
                    r.ok = i > 0;
                    break;
                }
                line = line_begin(s, line - 1);
            }
        }
        r.pos = std::min(line + col, line_end(s, line));
        break;
    }
    case 'w':
        for (int i = 0; i < count; ++i) {
            r.pos = word_next(s, r.pos);
        }
        if (op && count == 1 && r.pos > le) {
            r.pos = le; // "dw" on the last word stops at the line's end
        }
        break;
    case 'b':
        for (int i = 0; i < count; ++i) {
            r.pos = word_prev(s, r.pos);
        }
        break;
    case 'e':
        for (int i = 0; i < count; ++i) {
            r.pos = word_end(s, r.pos);
        }
        r.inclusive = true;
        break;
    case '0':
        r.pos = lb;
        break;
    case '^':
        r.pos = first_nonblank(s, c);
        break;
    case '$': {
        size_t e = le;
        for (int i = 1; i < count && e < s.size(); ++i) {
            e = line_end(s, e + 1);
        }
        r.pos = e;
        break;
    }
    case 'G':
    case 'g': {
        r.linewise = true;
        size_t line = 0;
        if (counted || m == 'g') {
            int n = counted ? count : 1;
            for (int i = 1; i < n; ++i) {
                size_t e = line_end(s, line);
                if (e >= s.size()) {
                    break;
                }
                line = e + 1;
            }
        } else {
            line = line_begin(s, s.size());
        }
        r.pos = first_nonblank(s, line);
        break;
    }
    case 'f':
    case 't': {
        size_t p = c;
        for (int i = 0; i < count; ++i) {
            size_t f = s.find(arg, next(s, p));
            if (f == std::string::npos || f >= le) {
                r.ok = false;
                return r;
            }
            p = f;
        }
        r.pos = m == 'f' ? p : prev(s, p);
        r.inclusive = true;
        break;
    }
    case 'F':
    case 'T': {
        size_t p = c;
        for (int i = 0; i < count; ++i) {
            size_t f = p == 0 ? std::string::npos : s.rfind(arg, p - 1);
            if (f == std::string::npos || f < lb) {
                r.ok = false;
                return r;
            }
            p = f;
        }
        r.pos = m == 'F' ? p : next(s, p);
        break;
    }
    default:
        r.ok = false;
    }
    return r;
}

// Word under the caret ("iw"), plus the blanks after it for "aw".
std::pair<size_t, size_t> word_object(const std::string& s, size_t c, bool around) {
    size_t lb = line_begin(s, c), le = line_end(s, c);
    if (c >= le) {
        return {c, c};
    }
    int k = cls(s, c);
    size_t a = c, b = c;
    while (a > lb && cls(s, a - 1) == k && s[a - 1] != '\n') {
        --a;
    }
    while (b < le && cls(s, b) == k) {
        ++b;
    }
    if (around) {
        size_t e = b;
        while (e < le && (s[e] == ' ' || s[e] == '\t')) {
            ++e;
        }
        if (e > b) {
            b = e;
        } else {
            while (a > lb && (s[a - 1] == ' ' || s[a - 1] == '\t')) {
                --a;
            }
        }
    }
    return {a, b};
}

} // namespace

void Editor::begin(const std::string& s, size_t caret) {
    mode = Mode::Insert;
    pending_.clear();
    undo_.clear();
    redo_.clear();
    save(s, caret);
}

void Editor::save(const std::string& s, size_t caret) {
    if (!undo_.empty() && undo_.back().text == s) {
        return;
    }
    undo_.push_back({s, caret});
    if (undo_.size() > 500) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
}

void Editor::to_normal(const std::string& s, size_t& caret) {
    mode = Mode::Normal;
    pending_.clear();
    if (caret > line_begin(s, caret)) {
        caret = prev(s, caret);
    }
    caret = clamp(s, caret);
}

bool Editor::undo(std::string& s, size_t& caret) {
    while (!undo_.empty() && undo_.back().text == s) {
        undo_.pop_back(); // the snapshot taken right before nothing changed
    }
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back({s, caret});
    s = undo_.back().text;
    caret = clamp(s, undo_.back().caret);
    undo_.pop_back();
    return true;
}

bool Editor::redo(std::string& s, size_t& caret) {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back({s, caret});
    s = redo_.back().text;
    caret = clamp(s, redo_.back().caret);
    redo_.pop_back();
    return true;
}

std::pair<size_t, size_t> Editor::selection(const std::string& s, size_t caret) const {
    size_t a = std::min(anchor, caret), b = std::max(anchor, caret);
    if (mode == Mode::VisualLine) {
        return {line_begin(s, a), line_end(s, b)};
    }
    return {a, next(s, b)};
}

bool Editor::key(std::string& s, size_t& c, std::string_view ch) {
    pending_ += ch;
    const std::string p = pending_;
    const std::string before = s;
    bool visual = mode == Mode::Visual || mode == Mode::VisualLine;

    size_t i = 0;
    auto read_count = [&] {
        int n = 0;
        while (i < p.size() && std::isdigit((unsigned char)p[i]) && !(p[i] == '0' && n == 0)) {
            n = std::min(n * 10 + (p[i++] - '0'), 9999);
        }
        return n;
    };
    int c1 = read_count();
    if (i >= p.size()) {
        return false; // just a count so far
    }
    char op = 0;
    if (!visual && std::strchr("dcy", p[i])) {
        op = p[i++];
    }
    int c2 = op ? read_count() : 0;
    if (i >= p.size()) {
        return false;
    }
    bool counted = c1 || c2;
    int count = std::max(c1, 1) * std::max(c2, 1);
    std::string_view rest = std::string_view(p).substr(i);
    char m = rest[0];
    bool object = (op || visual) && (m == 'i' || m == 'a');
    bool needs_arg = std::strchr("fFtTg", m) || object || (!op && !visual && m == 'r');
    if (needs_arg && rest.size() < 2) {
        return false;
    }
    std::string_view arg = needs_arg ? rest.substr(1) : std::string_view{};
    if (m == 'g' && arg != "g") {
        pending_.clear();
        return false;
    }
    pending_.clear();

    // Applies an operator to bytes [a, b).
    auto apply = [&](char o, size_t a, size_t b, bool lines) {
        if (lines) {
            size_t la = line_begin(s, a), le = line_end(s, b > a ? b - 1 : a);
            reg_ = s.substr(la, le - la);
            reg_lines_ = true;
            if (o == 'y') {
                return;
            }
            save(s, c);
            if (o == 'c') {
                s.erase(la, le - la);
                c = la;
                mode = Mode::Insert;
                return;
            }
            if (le < s.size()) {
                s.erase(la, le - la + 1);
            } else if (la > 0) {
                s.erase(la - 1, le - la + 1);
                la = line_begin(s, la - 1);
            } else {
                s.erase(la, le - la);
            }
            c = first_nonblank(s, std::min(la, s.size()));
            return;
        }
        reg_ = s.substr(a, b - a);
        reg_lines_ = false;
        if (o == 'y') {
            c = a;
            return;
        }
        save(s, c);
        s.erase(a, b - a);
        c = a;
        if (o == 'c') {
            mode = Mode::Insert;
        }
    };
    // Applies an operator over a motion from the caret.
    auto apply_motion = [&](char o, char mm, std::string_view a_, int n) {
        Motion mo = motion(s, c, mm, a_, n, counted, true);
        if (!mo.ok) {
            return;
        }
        if (o == 'c' && mm == 'w' && cls(s, c) != 0) { // "cw" acts like "ce"
            mo = motion(s, c, 'e', a_, n, counted, true);
        }
        size_t a = std::min(c, mo.pos), b = std::max(c, mo.pos);
        if (mo.linewise) {
            apply(o, line_begin(s, a), line_end(s, b) + 1, true);
            return;
        }
        if (mo.inclusive) {
            b = next(s, b);
        }
        apply(o, a, b, false);
    };

    if (op) {
        if (m == op) { // dd cc yy: whole lines
            size_t a = line_begin(s, c), e = a;
            for (int k = 0; k < count; ++k) {
                e = line_end(s, e);
                if (k + 1 < count && e < s.size()) {
                    ++e;
                }
            }
            size_t keep = c;
            apply(op, a, e + 1, true);
            if (op == 'y') {
                c = keep;
            }
        } else if (object) {
            if (arg == "w") {
                auto [a, b] = word_object(s, c, m == 'a');
                apply(op, a, b, false);
            }
        } else if (is_motion(m)) {
            apply_motion(op, m, arg, count);
        }
    } else if (visual) {
        auto [a, b] = selection(s, c);
        bool lines = mode == Mode::VisualLine;
        if (is_motion(m)) {
            Motion mo = motion(s, c, m, arg, count, counted, false);
            if (mo.ok) {
                c = mo.pos;
            }
        } else if (object) {
            if (arg == "w") {
                auto [wa, wb] = word_object(s, c, m == 'a');
                if (wb > wa) {
                    anchor = wa;
                    c = prev(s, wb);
                }
            }
        } else if (m == 'v' || m == 'V') {
            Mode want = m == 'v' ? Mode::Visual : Mode::VisualLine;
            mode = mode == want ? Mode::Normal : want;
        } else if (m == 'o') {
            std::swap(anchor, c);
        } else if (std::strchr("dxyc", m) || m == 's') {
            char o = m == 'x' ? 'd' : m == 's' ? 'c' : m;
            mode = Mode::Normal;
            apply(o, a, lines ? b + 1 : b, lines);
        } else if (m == 'J') {
            mode = Mode::Normal;
        }
    } else if (is_motion(m)) {
        Motion mo = motion(s, c, m, arg, count, counted, false);
        if (mo.ok) {
            c = mo.pos;
        }
    } else {
        size_t lb = line_begin(s, c), le = line_end(s, c);
        switch (m) {
        case 'x': apply_motion('d', 'l', {}, count); break;
        case 'X': apply_motion('d', 'h', {}, count); break;
        case 's': apply_motion('c', 'l', {}, count); break;
        case 'D': apply_motion('d', '$', {}, count); break;
        case 'C': apply_motion('c', '$', {}, count); break;
        case 'S': apply('c', lb, le + 1, true); break;
        case 'r':
            if (c < le) {
                save(s, c);
                s.replace(c, next(s, c) - c, arg);
            }
            break;
        case '~':
            if (c < le) {
                save(s, c);
                for (int k = 0; k < count && c < le; ++k) {
                    unsigned char ch_ = s[c];
                    s[c] = char(std::isupper(ch_) ? std::tolower(ch_) : std::toupper(ch_));
                    c = next(s, c);
                }
            }
            break;
        case 'J':
            if (le < s.size()) {
                save(s, c);
                size_t e = le + 1;
                while (e < s.size() && (s[e] == ' ' || s[e] == '\t')) {
                    ++e;
                }
                bool space = le > lb && e < s.size() && s[e] != '\n';
                s.replace(le, e - le, space ? " " : "");
                c = le;
            }
            break;
        case 'i':
            save(s, c);
            mode = Mode::Insert;
            break;
        case 'a':
            save(s, c);
            c = std::min(next(s, c), le);
            mode = Mode::Insert;
            break;
        case 'I':
            save(s, c);
            c = first_nonblank(s, c);
            mode = Mode::Insert;
            break;
        case 'A':
            save(s, c);
            c = le;
            mode = Mode::Insert;
            break;
        case 'o':
            save(s, c);
            s.insert(le, "\n");
            c = le + 1;
            mode = Mode::Insert;
            break;
        case 'O':
            save(s, c);
            s.insert(lb, "\n");
            c = lb;
            mode = Mode::Insert;
            break;
        case 'p':
        case 'P':
            if (reg_.empty() && !reg_lines_) {
                break;
            }
            save(s, c);
            if (reg_lines_) {
                if (m == 'p') {
                    s.insert(le, "\n" + reg_);
                    c = le + 1;
                } else {
                    s.insert(lb, reg_ + "\n");
                    c = lb;
                }
                c = first_nonblank(s, c);
            } else {
                size_t at = m == 'p' && c < le ? next(s, c) : c;
                s.insert(at, reg_);
                c = prev(s, at + reg_.size());
            }
            break;
        case 'u':
            for (int k = 0; k < count; ++k) {
                undo(s, c);
            }
            break;
        case 'v':
            mode = Mode::Visual;
            anchor = c;
            break;
        case 'V':
            mode = Mode::VisualLine;
            anchor = c;
            break;
        default:
            break;
        }
    }
    if (mode != Mode::Insert) {
        c = clamp(s, c);
    }
    return s != before;
}

} // namespace vim
