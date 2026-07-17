#pragma once

#include <QRegularExpression>
#include <QString>

// Why a user-authored regex won't compile, as Qt's own message — or an empty
// string if it's valid (an empty pattern is valid: every feature here reads it
// as "no constraint", never as "match nothing").
//
// Header-only leaf, deliberately owned by NEITHER caller: both the
// split-recording combine rules (TranscriptionController::regexValidationError())
// and the USB grab rules (UsbGrabberController::regexValidationError()) surface
// a live ✓/✗ indicator while the user types a pattern, and both must agree with
// the engine that will actually run it. Sharing one definition is what keeps
// "valid here" and "valid there" from drifting apart if the validation policy
// ever changes — the two subsystems otherwise share nothing, and routing one
// through the other's controller just for this would couple them for no reason.
//
// QRegularExpression, NOT QML/JS RegExp: the QML side must call down to this
// rather than reimplement the check, since the two syntaxes aren't guaranteed
// identical and a pattern that validates in JS could still fail at grab/combine
// time (asserted by tst_styleguide.cpp).
inline QString regexValidationErrorFor(const QString &pattern)
{
    if (pattern.isEmpty())
        return QString();
    const QRegularExpression re(pattern);
    return re.isValid() ? QString() : re.errorString();
}
