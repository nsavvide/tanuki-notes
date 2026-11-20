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
| Light up a flow or issue | C cycles through them, or click one in the panel (top right); click an issue tag on a box to open it |
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


## Notes as Markdown pages

Every box with a title is also a Markdown page, so you (or Claude) can write
the map as text and the drawing follows. Edits go both ways: type in the app
and the page updates; save a page and the open window updates within a
second. Boxes you have placed never move because of a page edit; new ones
are laid out automatically (along their arrows, or in a grid), and the box
grows and pushes what's below it down to make room.

```
notes/
  _index.md                  the top level: notes, `##` boxes, arrows
  qargo/_index.md            a box with a folder of its own
  qargo/architecture/_index.md
  qargo/hasura.md            a box with a page but no folder
  flows/order-to-invoice.md  a flow (see below)
```

A page:

```markdown
# Error Handling
issue: https://www.notion.so/team/Order-import-retries-0123…  Order import retries

- Do the work
- Fail if something went wrong (`UserInputValidationError`)

---

A second note. ```py fences become code cards.

-> [[Orchestration Service]]: calls

## Order Import Execution Service
### steps
```

- **`#`** is the box's title; **`##`, `###`…** are boxes inside it. A box
  with boxes of its own can also be a folder with an `_index.md`: same
  meaning, so moving a section into its own file is free.
- **Text** is the notes you read when you zoom in; `---` starts the next
  note. A note line that would read as structure (`## …`, `---`, an arrow)
  is written with a leading `\`.
- **Arrows** go from the box they're written in: `-> [[Box]]: label`, `-->`
  dashed, `..>` dotted, `<->` both ways, `--` / `..` lines. `[[Shard 1 >
  Primary]]` picks one of several boxes with the same title.
- **Issues:** `issue: <url> [title]` puts a tag on the box (click it to open
  the page, e.g. in Notion). The panel top right lists every issue; pick one
  and every box linked to it lights up.
- **Flows** are pages in `flows/`: a title, a description, and steps
  `[[A]] -> [[B]]: label`. They show as numbered arrows, only while picked
  in the panel (or with C).

Anything without a title (freehand, images, unlabelled shapes, arrows to a
note) lives only in `board.json`, next to its page.

`tanuki-notes --build <map>` reads the pages, places what's new, saves and
exits (exit 1 when an arrow or flow names a box that doesn't exist).
`TANUKI_SNAPSHOT_AT="Architecture" TANUKI_SNAPSHOT_FOCUS=flow:order-to-invoice
tanuki-notes --snapshot out.png <map>` renders a PNG zoomed onto a box, with
a flow (or `issue:<url>`) lit.

The style panel also has dashed and dotted strokes for hand-drawn shapes.

## The map folder (git-friendly)

Everything autosaves to plain files: the Markdown pages above, plus

```
notes/
  board.json                 where things are at the top level, and drawings
  qargo/board.json           the same inside "Qargo" (next to its _index.md)
  assets/                    copied images and PDFs
  .view.json                 where you were looking (gitignored)
```

Ids are stable and files are only rewritten when they change, so `git diff`
shows exactly what you edited. The app only ever deletes pages it wrote or
read itself. Ctrl+S runs `git init` (first time, if needed), `git add -A` and
`git commit` in the map folder.

A map folder inside an existing repo (say `myproject/docs/map`) uses that
repo instead of creating its own, and Ctrl+S commits only the map folder:
anything else you have staged is left alone.

An older map (boards only) turns into pages the first time it's opened.

## How infinite zoom works

Each shape has its own coordinate space (1 unit wide). The camera is always
measured in the units of the deepest shape that covers the window, and hands
itself to a child or parent as you zoom ("re-rooting"). Numbers never grow,
so there is no precision limit. See `App::reroot` in `src/app.cpp`.

## Licenses

Fonts in `assets/fonts`: Virgil (Excalidraw's hand-drawn font), Inter
(regular, medium, semibold, italic) and JetBrains Mono, all under the SIL Open Font License; see the license files
next to them.
