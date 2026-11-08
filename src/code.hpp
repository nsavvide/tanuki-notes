#pragma once

// Code blocks: the languages they know and a small syntax highlighter.
// ponytail: hand-rolled lexer (keywords, strings, comments, numbers, calls);
// switch to tree-sitter if it ever needs to be exact.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace code {

struct Lang {
    const char* name; // stored in board.json and shown on the block
    const char* ext;  // file extension for nvim (picks the filetype)
};

// Languages offered in the picker (the first is the default).
const std::vector<Lang>& languages();
const Lang& lang(std::string_view name); // unknown names fall back to "text"

enum class Tok { Plain, Keyword, Type, String, Number, Comment, Function };

struct Token {
    size_t begin, end; // bytes of the line
    Tok kind;
};

// Highlights one line. `in_block_comment` carries /* */ state across lines.
std::vector<Token> highlight(std::string_view lang, std::string_view line,
                             bool& in_block_comment);

// 0xRRGGBB colors on the dark card (One Dark).
constexpr uint32_t background = 0x282c34;
constexpr uint32_t muted = 0x7f848e;
uint32_t color(Tok t);

} // namespace code
