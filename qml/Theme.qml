pragma Singleton
import QtQuick

// STRUCTURAL palette — the colors that define the app's chrome rather than the
// user's preferences. The split from AppSettings is deliberate and load-bearing:
//
//   AppSettings.*Color  = USER-CONFIGURABLE, persisted per-vault, exposed in the
//                         settings UI with a color picker (accentColor,
//                         sidebarBackgroundColor, recordingActiveColor, ...).
//   Theme.*             = FIXED chrome, not user-facing. Changing one of these
//                         restyles the app; it is not a preference.
//
// Before this existed these were 649 raw hex literals spread over 12 files
// (#3a3a3a alone appeared 153 times), so a chrome tweak meant a 12-file
// find-and-replace and the copies had already drifted apart. `borderRadius`
// had been centralized on AppSettings much earlier and is referenced ~139
// times without trouble — this is that same idea applied to color.
//
// NOTE on the two same-valued pairs below: `border` and `surfaceHover` are both
// #3a3a3a today, and `danger` matches AppSettings.recordingActiveColor's default.
// They are kept as SEPARATE names because they are semantically different roles
// that happen to collide right now — collapsing them would mean a future tweak
// to one silently changes the others.
Item {
    id: theme

    // --- Surfaces ---------------------------------------------------------
    // Base fill of a button/chip at rest, and the lighter fill it takes on
    // hover. Together these two are the `hovered ? surfaceHover : surface`
    // pair that AppButton bakes in.
    readonly property color surface: "#2a2a2a"
    readonly property color surfaceHover: "#3a3a3a"
    // Backdrop of modal popups/confirm dialogs — deliberately darker than
    // `surface` so a dialog reads as sitting above the page.
    readonly property color surfaceDialog: "#242424"

    // --- Lines ------------------------------------------------------------
    // The 1px outline on essentially every framed element. This is the single
    // most-used value in the app (93 of its uses were `border.color`).
    readonly property color border: "#3a3a3a"

    // --- Text & icons -----------------------------------------------------
    // Default foreground for labels and AppIcon fills. Not pure white: white is
    // reserved for emphasis/selected states, which is why both exist.
    readonly property color textPrimary: "#dddddd"
    // Supporting text — captions, hints, secondary rows.
    readonly property color textSecondary: "#888888"
    // Inactive-but-available state, e.g. the unselected half of a toggle
    // (`checked ? "white" : textMuted`).
    readonly property color textMuted: "#aaaaaa"
    // Genuinely disabled. Paired with textPrimary in the recurring
    // `enabled ? textPrimary : textDisabled` idiom on icon buttons.
    readonly property color textDisabled: "#666666"

    // Lighter outline used for the "off" half of a toggle, where the "on" half
    // is `success` — a plain `border` there would read as disabled.
    readonly property color borderStrong: "#4a4a4a"

    // --- Semantic ---------------------------------------------------------
    // Errors and destructive actions (delete buttons, error snackbars).
    // Do NOT use this for the recording indicator — that is
    // AppSettings.recordingActiveColor, which the user can change.
    readonly property color danger: "#e07a7a"

    // Enabled/available/on. `successDim` is its at-rest partner, mirroring the
    // surface/surfaceHover pair: `checked ? (hovered ? success : successDim)`.
    readonly property color success: "#2ecc71"
    readonly property color successDim: "#25a25a"
}
