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
./build.sh install        # release build to ~/.local/bin/tanuki-notes
```

The fonts are compiled into the binary, so after `install` you can run
`tanuki-notes` from anywhere (default map: `~/tanuki`) or
`tanuki-notes path/to/map`.

## Using it

| Action | How |
|---|---|
| Zoom | scroll wheel, pinch |
| Pan | middle-drag, space + drag, hand tool, shift + scroll |
| Dive into a shape | double-click inside it (not on its label), Enter, or mouse forward button |
| Zoom out one level | Esc or Backspace (with nothing selected), mouse back button, or the level badge; the shape you left stays selected, so Esc again keeps going out |
| Write in nvim | `o` on a selected note/shape/code block (or `o` on nothing for a new note): opens nvim in a terminal, every `:w` updates the map |
| Code block | `c`, then write it in nvim; pick the language in the left panel |
| Fit everything | Home or Shift+1 |
| Jump to a parent | click it in the breadcrumb (top left) |
| Search | Ctrl+F or `/`: labels, notes, file names and text inside PDFs; ↑↓ and Enter fly there |
| Attach an arrow | start or end it on a shape; it follows when the shape moves or resizes |
| Detach an arrow | drag its end off the shape, or move the arrow by itself |
| Label a shape / edit text | double-click the label (or a blank new shape) |
| New text | double-click the background, or T |
| Insert image or PDF | 9, drag and drop a file, or paste an image |
| Move into / out of a shape | drag it inside or outside another shape |
| Save + git commit | Ctrl+S or the Commit button |

Tools: 1 select, 2 rectangle, 3 diamond, 4 ellipse, 5 arrow, 6 line,
7 pen, 8 text, 9 image/PDF, 0 eraser; hold Space to pan.

Vim keys: h/j/k/l select the nearest shape that way (Shift+hjkl moves it
10 px), i edit in place, o write in nvim, c new code block, y copy,
p paste, x delete, u undo, Ctrl+R redo.

Editing: Ctrl+Z / Ctrl+Shift+Z undo/redo, Ctrl+C/X/V, Ctrl+D duplicate,
Ctrl+A select all, Delete, arrow keys nudge (Shift = 10 px),
Ctrl+] / Ctrl+[ bring to front / send to back.

nvim opens in `$TERMINAL`, else the first of kitty, ghostty, foot,
alacritty or wezterm. Notes open as Markdown, code blocks with their
language's extension, so your nvim config (LSP, treesitter) applies.

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
shows exactly what you edited. Ctrl+S runs `git init` (first time, if needed),
`git add -A` and `git commit` in the map folder.

A map folder inside an existing repo (say `myproject/docs/map`) uses that
repo instead of creating its own, and Ctrl+S commits only the map folder:
anything else you have staged is left alone.

## How infinite zoom works

Each shape has its own coordinate space (1 unit wide). The camera is always
measured in the units of the deepest shape that covers the window, and hands
itself to a child or parent as you zoom ("re-rooting"). Numbers never grow,
so there is no precision limit. See `App::reroot` in `src/app.cpp`.

## Licenses

Fonts in `assets/fonts`: Virgil (Excalidraw's hand-drawn font), Inter and
JetBrains Mono, all under the SIL Open Font License; see the license files
next to them.
