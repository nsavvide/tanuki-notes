#pragma once

// Automatic placement for things that come from the Markdown pages without a
// position yet (w <= 0). Placed nodes never move: once a box is where you
// want it, it stays there.

#include "node.hpp"

struct NVGcontext;

namespace layout {

// A level where everything is new gets a full layout (layered along its
// arrows, or a grid, with its notes on top); new things in a level that
// already has some go below what's there, at the same scale. Arrows without
// points get a straight line between their shapes.
void place_new(NVGcontext* vg, Node& root);

} // namespace layout
