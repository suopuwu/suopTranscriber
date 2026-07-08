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

## Extending this doc

This file is the place for future UI convention rules (spacing, typography,
iconography, animation timing, etc.) as the app grows. Keep entries short
and rule-shaped, not prose descriptions of specific features.
