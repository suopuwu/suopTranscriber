#include "Task.h"

// Task is an abstract Q_OBJECT with only inline bodies — this translation
// unit exists purely so AUTOMOC has a header/source pair to generate and
// link Task's moc output against (every other QObject in this codebase
// follows the same one-.cpp-per-QObject-header convention).
