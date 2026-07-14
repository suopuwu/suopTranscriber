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

## Always-editable text

- Display-vs-edit is never a swap between two different elements (a `Label`/
  read-only `TextEdit` that gets replaced by a `TextField`/`TextArea` on
  click). One editable control is present at all times, styled via
  `UnderlineField` (see "Text fields" above) so it reads as plain text until
  focused. A visibility flag tied to focus state is exactly the bug this
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

## Warning banners

- Inline, not a popup/toast: a plain `Label`, conditionally visible on a
  non-empty backing string property (empty = hidden, no separate `visible`
  flag to keep in sync).
- Color `#e07a7a` for an error/rejection (e.g. `speakerRenameError`),
  `#e0a25a` for a softer "didn't fully work" warning (e.g. a requested
  feature that degraded rather than failed outright).
- `font.pixelSize: 11`, `wrapMode: Text.WordWrap`, `Layout.fillWidth: true`.
- No dismiss button — the label disappears once its backing property clears
  (either automatically, e.g. a timer, or the next time the underlying state
  it reports on changes).

## Extending this doc

This file is the place for future UI convention rules (spacing, typography,
iconography, animation timing, etc.) as the app grows. Keep entries short
and rule-shaped, not prose descriptions of specific features.
