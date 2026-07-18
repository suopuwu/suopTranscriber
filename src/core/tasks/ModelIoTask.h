#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// Imports or exports a managed model's file(s) — a possibly multi-GB copy into
// or out of the app's model store (ModelManager::importModel/exportModel).
//
// Serial lane: per the lane rule, anything INTENSIVE on system resources runs
// on the one serial queue — that's no longer just "uses the GPU/a warm model"
// but also a heavy CPU/IO job like this multi-GB copy, so it can't thrash the
// disk against a running transcription. Background presentation: it's launched
// from the Models settings page, not the main panel, so it earns a Tasks row +
// the glow but never raises the main-panel overlay. Replaces the old
// fire-and-forget QThread in importModel()/exportModel(), which had NO Tasks
// row at all — exactly the invisible-work failure this refactor removes.
class ModelIoTask : public Task {
    Q_OBJECT
public:
    enum class Direction { Import, Export };

    // Import: path is the user-supplied SOURCE (a file for single-file models,
    // a folder for the multi-file bundles). Export: path is the DESTINATION
    // directory. modelId is the managed model id either way.
    ModelIoTask(Direction direction, QString modelId, QUrl path, QObject *parent = nullptr);

    QString kind() const override;      // "importModel" / "exportModel"
    QString kindLabel() const override; // "Import model" / "Export model"
    QString displayName() const override;
    Lane lane() const override { return Lane::Serial; }
    Presentation presentation() const override { return Presentation::Background; }
    // The copy is a single blocking ModelManager call with no mid-pass abort
    // hook, so a cancel only takes effect at the next task boundary (like
    // rename/merge): not pausable, not cancellable mid-run.
    Capabilities capabilities() const override { return {false, false}; }
    // Import adds a model to the store (Export doesn't) — so a successful
    // import is what refreshes the Models page, via the one central
    // modelsChanged() emit keyed off this hook.
    bool completionChangesModelSet() const override { return m_direction == Direction::Import; }
    // Not persisted: an import's source may be an external/removable path, and
    // both directions are in-session one-shot actions (same as ImportTask).
    QVariantMap toVariantMap() const override;

    void run(TaskContext &ctx) override;

signals:
    // op is "import"/"export"; the controller re-emits modelOperationFinished()
    // so the Models page can report the outcome. The models-list refresh
    // itself (modelsChanged()) rides the central completionChangesModelSet()
    // hook, not this signal.
    void modelIoFinished(const QString &op, const QString &modelId, bool ok, const QString &error);

private:
    Direction m_direction;
    QString m_modelId;
    QUrl m_path;
    QString opName() const;
};
