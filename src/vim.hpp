#pragma once

// Vim-style editing of a text buffer: Insert, Normal and Visual modes.
// The app inserts typed text itself in Insert mode; in the other modes every
// typed character goes through Editor::key.
//
// Normal: h j k l w b e 0 ^ $ gg G f F t T, counts, d c y with any motion
// (dd cc yy, iw aw), x X s S D C r J ~, i a I A o O, p P, u (Ctrl+R redo),
// v V. Visual: motions, then d x y c s.
// ponytail: no registers, marks, macros, dot-repeat or ex commands.

#include <string>
#include <string_view>
#include <vector>

namespace vim {

enum class Mode { Insert, Normal, Visual, VisualLine };

class Editor {
  public:
    Mode mode = Mode::Insert;
    size_t anchor = 0; // where Visual mode started

    // Starts editing a buffer (in Insert mode).
    void begin(const std::string& s, size_t caret);
    // Insert -> Normal (Esc): the caret steps back onto the last character.
    void to_normal(const std::string& s, size_t& caret);
    // One typed character (a UTF-8 code point) outside Insert mode.
    // Returns true if the text changed.
    bool key(std::string& s, size_t& caret, std::string_view ch);
    bool undo(std::string& s, size_t& caret);
    bool redo(std::string& s, size_t& caret);

    // Visual selection as bytes [first, last).
    std::pair<size_t, size_t> selection(const std::string& s, size_t caret) const;
    const std::string& pending() const { return pending_; }
    void cancel() { pending_.clear(); } // Esc in the middle of a command

  private:
    void save(const std::string& s, size_t caret);

    std::string pending_;    // keys of the command being typed
    std::string reg_;        // the one yank register
    bool reg_lines_ = false; // yanked whole lines
    struct Snap {
        std::string text;
        size_t caret;
    };
    std::vector<Snap> undo_, redo_;
};

} // namespace vim
