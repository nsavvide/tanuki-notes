# tanuki-notes

An infinitely zoomable, hand-drawn knowledge map. It looks and feels like
Excalidraw, but every shape is also a world of its own: scroll into it and
keep drawing inside, as deep as you like.

## Build and run

Needs CMake, Ninja, a C++23 compiler, OpenGL 3.3 and poppler-cpp
(`pacman -S poppler`). SDL3, NanoVG and nlohmann/json are fetched by CMake.

```bash
./build.sh run            # opens ~/tanuki
./build.sh run ~/notes    # opens another map folder
./build.sh test           # self-test (hidden window)
./build.sh release        # optimized build in build-release/
```

## Using it

| Action | How |
|---|---|
| Zoom | scroll wheel, pinch |
| Pan | middle-drag, space + drag, hand tool, shift + scroll |
| Dive into a shape | select it, Enter |
| Zoom out one level | Esc (with nothing selected), or click the level badge |
| Fit everything | Home or Shift+1 |
| Jump to a parent | click it in the breadcrumb (top left) |
| Search | Ctrl+F or `/`: labels, notes, file names and text inside PDFs; ↑↓ and Enter fly there |
| Attach an arrow | start or end it on a shape; it follows when the shape moves or resizes |
| Detach an arrow | drag its end off the shape, or move the arrow by itself |
| Label a shape / edit text | double-click |
| New text | double-click empty space, or T |
| Insert image or PDF | 9, drag and drop a file, or paste an image |
| Move into / out of a shape | drag it inside or outside another shape |
| Save + git commit | Ctrl+S or the Commit button |

Tools: H hand, V/1 select, R/2 rectangle, D/3 diamond, O/4 ellipse,
A/5 arrow, L/6 line, P/7 pen, T/8 text, 9 image/PDF, E/0 eraser.

Editing: Ctrl+Z / Ctrl+Shift+Z undo/redo, Ctrl+C/X/V, Ctrl+D duplicate,
Ctrl+A select all, Delete, arrow keys nudge (Shift = 10 px),
Ctrl+] / Ctrl+[ bring to front / send to back.

## The map folder (git-friendly)

Everything autosaves to plain files:

```
~/tanuki/
  board.json                 shapes at the top level
  rust-3f9a1c/board.json     shapes inside the "Rust" shape
  rust-3f9a1c/ownership-.../ ...nested as deep as you zoom
  assets/                    copied images and PDFs
  .view.json                 where you were looking (gitignored)
```

Ids are stable and files are only rewritten when they change, so `git diff`
shows exactly what you edited. Ctrl+S runs `git init` (first time),
`git add -A` and `git commit` in the map folder.

## How infinite zoom works

Each shape has its own coordinate space (1 unit wide). The camera is always
measured in the units of the deepest shape that covers the window, and hands
itself to a child or parent as you zoom ("re-rooting"). Numbers never grow,
so there is no precision limit. See `App::reroot` in `src/app.cpp`.

## Licenses

Fonts in `assets/fonts`: Virgil (Excalidraw's hand-drawn font) and Inter,
both under the SIL Open Font License; see the license files next to them.
