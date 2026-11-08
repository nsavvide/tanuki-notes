#include "code.hpp"

#include <cctype>
#include <cstring>
#include <set>

namespace code {

namespace {

struct Syntax {
    const char* line_comment;  // nullptr: none
    bool block_comments;       // /* ... */
    const char* quotes;        // characters that start a string
    std::set<std::string_view> keywords, types;
};

const Syntax& syntax(std::string_view lang) {
    static const Syntax cpp{
        "//", true, "\"'",
        {"auto", "break", "case", "catch", "class", "const", "constexpr",
         "continue", "default", "delete", "do", "else", "enum", "explicit",
         "export", "for", "friend", "if", "import", "inline", "module",
         "namespace", "new", "noexcept", "nullptr", "operator", "private",
         "protected", "public", "return", "sizeof", "static", "struct",
         "switch", "template", "this", "throw", "try", "typedef", "typename",
         "union", "using", "virtual", "while", "true", "false", "co_await",
         "co_return", "co_yield", "concept", "requires", "mutable", "override",
         "final", "#include", "#define", "#pragma", "#if", "#ifdef", "#ifndef",
         "#endif", "#else", "#elif"},
        {"void", "bool", "char", "int", "long", "short", "float", "double",
         "unsigned", "signed", "size_t", "int8_t", "int16_t", "int32_t",
         "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "std"}};
    static const Syntax rust{
        "//", true, "\"",
        {"as", "async", "await", "break", "const", "continue", "crate", "dyn",
         "else", "enum", "extern", "false", "fn", "for", "if", "impl", "in",
         "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return",
         "self", "Self", "static", "struct", "super", "trait", "true", "type",
         "unsafe", "use", "where", "while"},
        {"i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64",
         "u128", "usize", "f32", "f64", "bool", "char", "str", "String", "Vec",
         "Option", "Result", "Box"}};
    static const Syntax python{
        "#", false, "\"'",
        {"and", "as", "assert", "async", "await", "break", "class", "continue",
         "def", "del", "elif", "else", "except", "False", "finally", "for",
         "from", "global", "if", "import", "in", "is", "lambda", "None",
         "nonlocal", "not", "or", "pass", "raise", "return", "True", "try",
         "while", "with", "yield", "match", "case"},
        {"int", "float", "str", "bool", "list", "dict", "set", "tuple", "bytes",
         "object", "self"}};
    static const Syntax js{
        "//", true, "\"'`",
        {"async", "await", "break", "case", "catch", "class", "const",
         "continue", "default", "delete", "do", "else", "export", "extends",
         "false", "finally", "for", "from", "function", "if", "import", "in",
         "instanceof", "interface", "let", "new", "null", "of", "return",
         "static", "super", "switch", "this", "throw", "true", "try", "type",
         "typeof", "undefined", "var", "void", "while", "yield", "enum",
         "implements", "private", "public", "readonly"},
        {"string", "number", "boolean", "any", "unknown", "never", "object"}};
    static const Syntax go{
        "//", true, "\"'`",
        {"break", "case", "chan", "const", "continue", "default", "defer",
         "else", "fallthrough", "for", "func", "go", "goto", "if", "import",
         "interface", "map", "package", "range", "return", "select", "struct",
         "switch", "type", "var", "nil", "true", "false"},
        {"int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16",
         "uint32", "uint64", "float32", "float64", "string", "bool", "byte",
         "rune", "error", "any"}};
    static const Syntax lua{
        "--", false, "\"'",
        {"and", "break", "do", "else", "elseif", "end", "false", "for",
         "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat",
         "return", "then", "true", "until", "while"},
        {}};
    static const Syntax sh{
        "#", false, "\"'",
        {"if", "then", "else", "elif", "fi", "for", "while", "do", "done",
         "case", "esac", "in", "function", "return", "local", "export",
         "echo", "cd", "set", "source"},
        {}};
    static const Syntax sql{
        "--", true, "'\"",
        {"select", "from", "where", "insert", "into", "values", "update", "set",
         "delete", "create", "table", "drop", "alter", "join", "left", "right",
         "inner", "outer", "on", "group", "by", "order", "having", "limit",
         "as", "and", "or", "not", "null", "is", "in", "primary", "key",
         "index", "distinct", "union", "with", "case", "when", "then", "else",
         "end", "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE",
         "SET", "DELETE", "CREATE", "TABLE", "DROP", "ALTER", "JOIN", "LEFT",
         "RIGHT", "INNER", "OUTER", "ON", "GROUP", "BY", "ORDER", "HAVING",
         "LIMIT", "AS", "AND", "OR", "NOT", "NULL", "IS", "IN", "PRIMARY",
         "KEY", "INDEX", "DISTINCT", "UNION", "WITH", "CASE", "WHEN", "THEN",
         "ELSE", "END"},
        {"int", "integer", "text", "varchar", "boolean", "timestamp", "uuid",
         "INT", "INTEGER", "TEXT", "VARCHAR", "BOOLEAN", "TIMESTAMP", "UUID"}};
    static const Syntax json{nullptr, false, "\"", {"true", "false", "null"}, {}};
    static const Syntax yaml{"#", false, "\"'", {"true", "false", "null"}, {}};
    static const Syntax plain{nullptr, false, "", {}, {}};

    if (lang == "cpp" || lang == "c" || lang == "glsl" || lang == "java" ||
        lang == "cs") {
        return cpp;
    }
    if (lang == "rust") return rust;
    if (lang == "python") return python;
    if (lang == "javascript" || lang == "typescript") return js;
    if (lang == "go") return go;
    if (lang == "lua") return lua;
    if (lang == "bash") return sh;
    if (lang == "sql") return sql;
    if (lang == "json") return json;
    if (lang == "yaml" || lang == "toml") return yaml;
    return plain;
}

bool ident_start(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool ident_char(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

} // namespace

const std::vector<Lang>& languages() {
    static const std::vector<Lang> all = {
        {"cpp"},     {"rust"},       {"python"},
        {"typescript"}, {"javascript"}, {"go"},
        {"lua"},     {"bash"},       {"sql"},
        {"json"},   {"yaml"},     {"c"},
        {"text"},
    };
    return all;
}

const Lang& lang(std::string_view name) {
    for (const Lang& l : languages()) {
        if (name == l.name) {
            return l;
        }
    }
    return languages().back();
}

std::string from_fence(std::string_view tag) {
    std::string t;
    for (char ch : tag) {
        if (ch != ' ' && ch != '\t') {
            t += char(std::tolower((unsigned char)ch));
        }
    }
    static const std::pair<const char*, const char*> alias[] = {
        {"py", "python"}, {"rs", "rust"},   {"js", "javascript"}, {"jsx", "javascript"},
        {"ts", "typescript"}, {"tsx", "typescript"}, {"sh", "bash"}, {"shell", "bash"},
        {"zsh", "bash"}, {"c++", "cpp"},    {"cc", "cpp"},        {"cxx", "cpp"},
        {"hpp", "cpp"},  {"h", "c"},        {"yml", "yaml"},      {"golang", "go"},
        {"txt", "text"}, {"", "text"}};
    for (auto [a, b] : alias) {
        if (t == a) {
            return b;
        }
    }
    return lang(t).name;
}

std::vector<Token> highlight(std::string_view lang, std::string_view s,
                             bool& in_block) {
    const Syntax& sx = syntax(lang);
    std::vector<Token> out;
    auto push = [&](size_t b, size_t e, Tok k) {
        if (e <= b) {
            return;
        }
        if (!out.empty() && out.back().kind == k && out.back().end == b) {
            out.back().end = e; // merge runs of the same color
        } else {
            out.push_back({b, e, k});
        }
    };
    size_t i = 0, n = s.size();
    while (i < n) {
        if (in_block) {
            size_t end = s.find("*/", i);
            size_t stop = end == std::string_view::npos ? n : end + 2;
            push(i, stop, Tok::Comment);
            in_block = end == std::string_view::npos;
            i = stop;
            continue;
        }
        unsigned char c = s[i];
        if (sx.line_comment && s.substr(i).starts_with(sx.line_comment)) {
            push(i, n, Tok::Comment);
            break;
        }
        if (sx.block_comments && s.substr(i).starts_with("/*")) {
            in_block = true;
            push(i, i + 2, Tok::Comment);
            i += 2;
            continue;
        }
        if (c && std::strchr(sx.quotes, c)) {
            // Rust lifetimes ('a) aren't strings; a quote with no closing
            // partner on the line is just punctuation.
            size_t j = i + 1;
            while (j < n && s[j] != c) {
                j += s[j] == '\\' ? 2 : 1;
            }
            if (j >= n) {
                push(i, i + 1, Tok::Plain);
                ++i;
                continue;
            }
            push(i, j + 1, Tok::String);
            i = j + 1;
            continue;
        }
        if (std::isdigit(c)) {
            size_t j = i;
            while (j < n && (ident_char(s[j]) || s[j] == '.')) {
                ++j;
            }
            push(i, j, Tok::Number);
            i = j;
            continue;
        }
        if (ident_start(c) || (c == '#' && i + 1 < n && ident_start(s[i + 1]))) {
            size_t j = i + 1;
            while (j < n && ident_char(s[j])) {
                ++j;
            }
            std::string_view w = s.substr(i, j - i);
            size_t k = j;
            while (k < n && s[k] == ' ') {
                ++k;
            }
            Tok t = Tok::Plain;
            if (sx.keywords.contains(w)) {
                t = Tok::Keyword;
            } else if (sx.types.contains(w) ||
                       (std::isupper((unsigned char)w[0]) && w.size() > 1 &&
                        !sx.keywords.empty())) {
                t = Tok::Type;
            } else if (k < n && (s[k] == '(' || (s[k] == '!' && lang == "rust"))) {
                t = Tok::Function;
            }
            push(i, j, t);
            i = j;
            continue;
        }
        push(i, i + 1, Tok::Plain);
        ++i;
    }
    return out;
}

uint32_t color(Tok t) {
    switch (t) {
    case Tok::Keyword: return 0xc678dd;
    case Tok::Type: return 0xe5c07b;
    case Tok::String: return 0x98c379;
    case Tok::Number: return 0xd19a66;
    case Tok::Comment: return muted;
    case Tok::Function: return 0x61afef;
    case Tok::Plain: break;
    }
    return 0xabb2bf;
}

} // namespace code
