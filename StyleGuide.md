# StyleGuide

UI/UX conventions for suopTranscriber's Qt6/QML front end. Terse, scannable,
same register as DESIGN.md. Add new conventions here as they're decided —
don't describe one-off feature implementation details.

## Interactable hover behavior

Every interactive element (buttons, tree rows, sliders, toolbar icons, etc.)
must show both:

- **Cursor**: `Qt.PointingHandCursor` on hover.
  - Custom `Item`s: `HoverHandler { cursorShape: Qt.PointingHandCursor }`.
  - QQC2 controls with built-in cursor support: `hoverEnabled: true`.
- **Outline**: thin 1px white border on hover.
  - Convention: reusable `qml/HoverBorder.qml` component — `Rectangle`,
    transparent fill, 1px white border, `radius: AppSettings.borderRadius`,
    opacity animated 0 → 1.
  - Overlay/anchor it on the interactive element; bind its `active` property
    to that element's own `hovered` state.

## Border radius

- One global setting: `AppSettings.borderRadius` (`qml/AppSettings.qml`).
- Use it everywhere corners are rounded — panels, popups, hover borders, the
  frameless window mask.
- Never hardcode a radius value ad hoc in a component.

## Settings

- Any new configurable app behavior is a property on the `AppSettings`
  singleton (`qml/AppSettings.qml`, backed by `Qt.labs.settings`).
- No bespoke storage mechanisms — keeps settings trivial to add/remove.
- Settings UI lives in `qml/SettingsPopup.qml`, bound bidirectionally to
  `AppSettings` properties.

## Color / background conventions

- Panel background = opaque base color + separate 0-1 opacity float,
  combined via `Qt.rgba` at paint time.
- Not a single RGBA color picker — keep color and transparency as
  independent controls for any panel-like surface.

## Text fields

- `TextField`s use an underline background (`qml/UnderlineField.qml`), not the
  default boxed background — `background: UnderlineField { focused:
  <id>.activeFocus }`. Thin/dim normally, thicker + `AppSettings.highlightColor`
  when focused.
- A **pure text box** — one that exists only to be typed into (the USB
  grabber's filename regex and start folder, search/filter fields, the numeric
  settings fields, the rename editors) — always shows its underline, dim (25%)
  at rest, so it reads as an input even when untouched. Never leave one with no
  underline at all: there's no surrounding prose for it to blend into, so a
  bare field just looks like a label. The three tiers are focused (100%) >
  hovered (50%) > at rest (25%).
- The only fields that hide the underline at rest are the always-editable ones
  below, via `plainUntilHovered: true`.

## Always-editable text

- Display-vs-edit is never a swap between two different elements (a `Label`/
  read-only `TextEdit` that gets replaced by a `TextField`/`TextArea` on
  click). One editable control is present at all times, styled via
  `UnderlineField { plainUntilHovered: true }` (see "Text fields" above) so it
  reads as plain text until hovered/focused. `plainUntilHovered` is for these
  fields ONLY — a pure text box keeps its resting underline. A visibility flag tied to focus state is exactly the bug this
  fixes: the platform's native right-click "Copy" menu steals focus, and if
  that flip hides the field, the box disappears out from under the user.
- If the pill/control ALSO needs a click-triggered action other than editing
  (e.g. filtering the sidebar by a speaker/tag pill) — don't gate the field's
  own editability behind a separate edit-trigger icon to make room for it.
  Overlapping `TapHandler`s (one on the field's container, one on a small
  icon inside it) aren't mutually exclusive by default in Qt Quick — a tap
  on the icon can fire both, which is exactly the "pencil button also
  triggers the pill's other action" bug this replaces. Instead, keep the
  field itself always directly editable (plain click places the cursor, same
  as everywhere else) and give the OTHER action its own small dedicated
  trigger icon (e.g. a `⇅` sort/filter `Text` + `TapHandler`) — never the
  field's own click.
- **Carve-out — inline rename in a list/tree row** (`Sidebar.qml`'s
  `rowRenameField`): a tree row genuinely can't be always-editable — a plain
  click on a row's name must open the file, not place a text cursor — so it
  IS a display-vs-edit swap. The rule's actual hazard (the field vanishing
  under a focus steal) is still barred: gate the swap on an explicit
  "which row is being renamed" property, **never on `activeFocus`**, so the
  native right-click "Copy" menu can't dismiss the editor. Blur still
  commits, but only once the field has actually held focus — entering edit
  mode is a frame ahead of focus landing, and a modal context menu closing
  releases focus later still; without that guard the opening frame reads as
  a blur and commits instantly. Applies only to rows in a list/tree; a
  panel field is always-editable per the rule above.
- Commit convention:
  - Single-line fields (title, a name, a tag): commit on `Return`
    (`onAccepted`) AND on blur (`onActiveFocusChanged: if (!activeFocus)
    commit(...)`) — safe once the field can no longer disappear.
    `Keys.onEscapePressed` reverts the text to the last-known-good value
    without committing.
  - Multi-line/structured fields (a paragraph, or text where each line maps
    to something else, e.g. one transcript line per timestamped segment):
    commit on blur only, and block `Return` from inserting a newline
    (`Keys.onReturnPressed: (event) => event.accepted = true`) if the
    structure depends on a fixed line count. Validate that invariant
    defensively at commit time too (a paste can slip a newline past the key
    handler) and reject + revert with an inline warning banner rather than
    writing a corrupted result.

## Feedback: inline banner vs. snackbar

Two surfaces, and the split is **what the message is about**, not how bad it
is. Both use the same colors (below), so the same class of problem reads the
same wherever it lands.

- **Describes the state of a control you're looking at** → inline banner,
  beside that control. An invalid regex belongs next to the search bar; an
  out-of-vault folder next to its picker. Moving these to a screen corner
  breaks the association with the field they explain.
- **Reports that an action just happened/failed, and is over** → snackbar. A
  refused import, move, rename, or delete has no control to sit beside — the
  row just stays put. These float bottom-right and auto-expire.

The tell: if the message must survive as long as some state does (the open
file's stored metadata, a field's current contents), it's inline. If it
describes a moment that has already passed, it's a snackbar.

### Warning banners (inline)

- A plain `Label`, conditionally visible on a non-empty backing string
  property (empty = hidden, no separate `visible` flag to keep in sync).
- `font.pixelSize: 11`, `wrapMode: Text.WordWrap`, `Layout.fillWidth: true`.
- No dismiss button — the label disappears once its backing property clears
  (either automatically, e.g. a timer, or the next time the underlying state
  it reports on changes).

### Snackbars (floating)

- **Never instantiate `Snackbar.qml` at a call site.** Post to the
  `Notifications` singleton: `Notifications.error(qsTr("Couldn't move: …"))`,
  or `.warn()` / `.info()` / `.post(cfg)` for actions and a custom timeout.
- One `SnackbarHost` exists, in `Main.qml`; it renders the queue bottom-right
  over `Overlay.overlay`. Don't add a second, and don't parent snackbars
  anywhere else — the whole point is that every floating message lands in one
  place the user learns to look at.
- Actions are `[{ label, onTriggered }]` on `post()`. An action is terminal:
  firing it dismisses the entry. Use one for a **reversal** (`Undo`) or a
  **retry**, not as a general button rack.
- Action/dismiss controls are `Item` + `TapHandler`, never a QQC2 `Button` or
  `MouseArea` — a focusable control would steal focus from a `TextField`
  behind the snackbar, and fields commit on blur here (see "Always-editable
  text"), so a press could fire a commit whose failure posts another snackbar
  mid-click.
- Entries auto-expire (default 6s, `timeout: 0` for sticky) and pause while
  the pointer is over the stack. An action never has to race its own timer.
- No confirmations and no successes that speak for themselves — a renamed row
  visibly renames itself. Post only what is otherwise silent.

## Destructive actions: trash first, confirm only what's permanent

- **Never confirm an action you can undo.** A delete tries the trash first; if
  that works, it just happens, and a snackbar offers `Undo`. A prompt on a
  reversible action is friction that teaches the user to click through
  prompts — which is what makes the one that matters get clicked through too.
- **A confirmation means "this cannot be undone", and only that.** The only
  delete prompt left (`fileDeleteConfirmPopup`) is reached solely for entries
  the trash refused, and its wording says so.
- **Don't predict whether the trash will work — try it.** A failed
  `QFile::moveToTrash()` is non-destructive (see `trashOnly()` in
  `VaultTreeModel.cpp`), so attempting it *is* the test, and the entries that
  fail are still on disk, untouched, ready to prompt about. Predicting would
  mean reimplementing the freedesktop spec and hoping it matches Qt's.
- Recoverable and permanent deletion are **separate model calls**
  (`trashFiles()` vs `deleteFilesPermanently()`), never one call that silently
  picks. A UI that can't tell which it got has to assume the worst every time.

### Colors (both surfaces)

- `#e07a7a` for an error/rejection, `#e0a25a` for a softer "didn't fully
  work" warning (e.g. a requested feature that degraded rather than failed
  outright). A snackbar's `kind` (`"error"` / `"warning"` / `"info"`) selects
  these; `"info"` uses `AppSettings.accentColor`.

## Iconography

- **Every icon is an `AppIcon`.** Never a bare glyph `Text`, a `Canvas`, an
  `icon.source`, or an `Image` at a call site. `AppIcon { name: "transcribe";
  iconSize: 15; color: "#dddddd" }` — name it semantically; which pack draws
  it is `IconTheme`'s decision (enforced by `tst_styleguide.cpp`'s
  `iconography_appIconUsedOnEveryIconSurface`).
- **Name the meaning, never the glyph.** `name: "delete-file"`, not a `⌦`.
  The semantic names are the keys in `assets/icons/<pack>/pack.json` — see
  `IconTheme::iconNames()`. Adding a UI action means adding its name to the
  pack manifests, not picking a character.
- **`iconSize` is the icon's box, not a font size.** Per-glyph optical
  correction belongs in a pack manifest's per-icon `scale`, never at the call
  site. Pass one canonical size per cluster (15 for the 22×22 ribbon buttons,
  13 for the 20×20 per-row hover clusters).
- **Color via the `color` property**, same enabled/hover expressions as
  before (`enabled ? "#dddddd" : "#666666"`; `#e07a7a` for destructive). The
  provider tints svg packs to it; glyph packs set `Text.color`.
- **Wrapper unchanged.** An icon still lives inside its `Item` +
  `HoverHandler` + `TapHandler` + `HoverBorder` (hover convention above) —
  only the inner icon element is an `AppIcon`.
- **Not every symbol is an icon.** Status dots (a `Rectangle` circle), a
  `✓`/`⚠` glyph used as a text *prefix* inside a larger label, and genuinely
  multi-glyph mode toggles (`.*`, `Aa`) stay as they are.
- New drawn (non-font, non-svg) icons are `builtin` pack entries pointing at
  a small `Canvas` component (see `PlayGlyph.qml`), registered in `AppIcon`'s
  closed `builtins` map — never an arbitrary component URL a pack names.
- Full architecture: DESIGN.md "Iconography".

## Extending this doc

This file is the place for future UI convention rules (spacing, typography,
iconography, animation timing, etc.) as the app grows. Keep entries short
and rule-shaped, not prose descriptions of specific features.
