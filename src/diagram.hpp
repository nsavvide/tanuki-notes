#pragma once

// Diagrams as text. A `*.tanuki` file in the map folder describes boxes,
// nesting, notes, arrows and concepts; build() turns it into nodes with an
// automatic layout, inside one top-level frame. The app rebuilds the frame
// whenever the file changes. The language is documented in README.md.

#include "node.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct NVGcontext;

namespace diagram {

// A named set of nodes and arrows that light up together (a flow through the
// whole drawing). Arrows declared inside a concept are its numbered steps.
struct Concept {
    std::string id, name, note;
    uint32_t color = 0; // 0xRRGGBBAA
};

struct Result {
    // The frame holding the diagram, 1 unit wide (sizes of the frame itself
    // assume its parent's units equal its width: scale them by the real w).
    // Null on error.
    std::unique_ptr<Node> frame;
    std::vector<Concept> concepts;
    std::string error; // "line 12: unknown box 'apii'"
};

// `name`: the file name; ids are derived from it, so rebuilding an unchanged
// file gives identical nodes (quiet git diffs).
Result build(NVGcontext* vg, const std::string& source, const std::string& name);

// Stable 12-hex-digit id for a string.
std::string hash_id(const std::string& s);

} // namespace diagram
