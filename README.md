# tanuki-notes

An infinitely zoomable, hand-drawn knowledge map. It looks and feels like
Excalidraw, but every shape is also a world of its own: scroll into it and
keep drawing inside, as deep as you like.

## Build and run

Needs CMake, Ninja, a C++23 compiler, OpenGL 3.3 and poppler-cpp
(`pacman -S poppler`). SDL3, NanoVG and nlohmann/json are fetched by CMake.

```bash
./build.sh run ~/notes    # opens (or creates) the map in ~/notes
./build.sh test           # self-test (hidden window)
./build.sh release        # optimized build in build-release/
./build.sh install        # release build to ~/.local/bin/tanuki-notes
```

The fonts are compiled into the binary, so after `install` you can run
`tanuki-notes` inside any folder to use it as the map (like `git` or
`nvim`), or `tanuki-notes path/to/map`. It refuses to turn your home
folder or `/` into a map.

## Using it

| Action | How |
|---|---|
| Zoom | scroll wheel, pinch |
| Pan | middle-drag, space + drag, hand tool, shift + scroll |
| Dive into a shape | double-click inside it (not on its label), Enter, or mouse forward button |
| Zoom out one level | Esc or Backspace (with nothing selected), mouse back button, or the level badge; the shape you left stays selected, so Esc again keeps going out |
| Code block | in a note or label, type ```` ```py ```` (or rs, cpp, ts, …) and Enter; ```` ``` ```` + Enter ends it. Pasted fenced Markdown becomes code blocks when you finish editing. Change the language in the left panel |
| Link to a note | type `[[` and pick a title (↑↓, Enter/Tab); click the link to fly there. Links match a note's or shape's first line, or a file name |
| Fit everything | Home or Shift+1 |
| Jump to a parent | click it in the breadcrumb (top left) |
| Search | Ctrl+F or `/`: labels, notes, file names and text inside PDFs; ↑↓ and Enter fly there |
| Attach an arrow | start or end it on a shape; it follows when the shape moves or resizes |
| Highlight a concept | C cycles through them, or click one in the Concepts panel |
| Dark mode | moon/sun button top right, or Shift+Alt+D; follows the system theme until you pick one |
| Pin an arrow to a spot | drop its end on or near the outline: it stays on that exact spot (snaps to edge midpoints); drop it deep inside to aim at the center |
| Detach an arrow | drag its end off the shape, or move the arrow by itself |
| Label a shape / edit text | double-click the label (or a blank new shape), or Enter on a selected note. Once a shape has things inside, its label becomes a title just above it |
| Label an arrow or line | double-click it (or select it and press Enter) |
| Curve an arrow or line | select it and drag the small dot halfway along; drag a bend back straight to remove it |
| New text | double-click the background, or T |
| Insert image or PDF | 9, drag and drop a file, or paste an image |
| Move into / out of a shape | drag it inside or outside another shape |
| Save + git commit | Ctrl+S or the Commit button |

Tools: H hand, V/1 select, R/2 rectangle, D/3 diamond, O/4 ellipse,
A/5 arrow, L/6 line, P/7 pen, T/8 text, 9 image/PDF, E/0 eraser.

Notes read like Notion: Inter on a fixed page width (drag a note's side
handles to change it) and Markdown that renders as you write: `#`/`##`/`###`
headings, `-` bullets, `1.` numbered lists, `- [ ]` checkboxes (click to
tick), `>` quotes, `---` dividers, `**bold**`, `*italic*`, `` `code` ``,
`~~strike~~`. Markers show faintly only on the line you're typing in. Enter
continues a list, Enter on an empty item ends it, Tab / Shift+Tab indent,
Backspace at an item's start turns it back into text. Shape labels keep the
hand-drawn font.

Writing is vim: you start in Insert mode; Esc goes to Normal mode, Esc
again (or a click elsewhere) finishes. Normal mode has h j k l w b e 0 ^ $
gg G f F t T with counts, d c y with any motion (dd, cw, ciw, d$, 3dw…),
x X s S D C r J ~, i a I A o O, p P, u and Ctrl+R, and v / V visual mode.
The mode shows at the bottom of the window.

Editing: Ctrl+Z / Ctrl+Shift+Z undo/redo, Ctrl+C/X/V, Ctrl+D duplicate,
Ctrl+A select all, Delete, arrow keys nudge (Shift = 10 px),
Ctrl+] / Ctrl+[ bring to front / send to back.


## Diagrams as code (`*.tanuki`)

Write a diagram as text and the app draws it: every `*.tanuki` file in the
map folder becomes one frame at the top level, laid out automatically. Save
the file and the open window updates within half a second. Move or resize
the frame as you like (it stays put), and anything you draw by hand inside
it survives rebuilds. Everything else in the frame comes from the file, so
edit the file, not the shapes.

```
# Qargo platform                         title of the frame
> Markdown note shown inside the frame.  notes: "> " lines

concept order: Order lifecycle [orange]  a concept (highlight set)
  > From EDI import to invoice.           its explanation
  edi -> api: EDI message                 arrows inside it are numbered steps
  api -> orders: create order

edi: EDI inbox [cyan]                    a box: id: Label [attrs]
api: API gateway [#order]                #concept tags it
  > Auth + routing.                       a note inside: zoom in to read it
  > ```py                                 fenced code becomes a code card
  > def handler(): ...
  > ```
services: Services [down]                indented boxes go inside (zoomable)
  orders: Orders
  invoices: Invoices
  orders --> invoices: nightly           dashed arrow
pg: Postgres\n(primary) [ellipse, green]  \n breaks a label
api ..> pg: reads                         dotted arrow
api <-> orders                            heads at both ends
api -- pg                                 plain line; `..` dotted line
```

- **Arrows** connect any two boxes by id, at any depth; they live in the
  deepest box holding both ends. Chains work: `a -> b -> c`.
- **Box attrs:** `ellipse`, `diamond`, color (`red pink grape violet indigo
  blue cyan teal green lime yellow orange gray white none`), `dashed`,
  `dotted`, `hatch`, `crosshatch`, `clean`, `sketchy`, `bold`, `thin`,
  `big`, layout of what's inside (`right`, `down`, `grid`), `#concept`.
- **Arrow attrs:** color, `dashed`, `dotted`, `bold`, `thin`, `#concept`.
- **Layout:** boxes linked by arrows flow left to right (or `down`); boxes
  without arrows form a grid. `layout down` on its own line works too.
- **Concepts:** the panel top right lists them; click one (or press C to
  cycle) and everything else dims while its boxes, arrows and the shapes
  containing them glow. Steps are numbered in order.
- `// comment` lines are ignored. Errors show as a toast with the line.

`tanuki-notes --build <map>` rebuilds without a window (exit 1 on errors).
`TANUKI_SNAPSHOT_AT="API gateway" TANUKI_SNAPSHOT_CONCEPT=order
tanuki-notes --snapshot out.png <map>` renders a PNG zoomed onto a shape,
with a concept lit.

The style panel also has dashed and dotted strokes for hand-drawn shapes.

## The map folder (git-friendly)

Everything autosaves to plain files:

```
notes/
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

Fonts in `assets/fonts`: Virgil (Excalidraw's hand-drawn font), Inter
(regular, medium, semibold, italic) and JetBrains Mono, all under the SIL Open Font License; see the license files
next to them.
