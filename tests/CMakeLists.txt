# Test suite = living spec sheet. Each tst_* documents expected behaviors from
# DESIGN.md / CLAUDE.md / StyleGuide.md. Documented-but-unbuilt behaviors live
# in tst_future_spec as QSKIP placeholders so every ctest run lists them.

find_package(Qt6 6.7 REQUIRED COMPONENTS Test)

set(SUOP_CORE_DIR ${CMAKE_SOURCE_DIR}/src/core)

# Wave N (task system): TranscriptionController.cpp's connectTask() references
# every concrete Task subclass (qobject_cast per kind), so ANY test that
# compiles the controller must also compile the whole tasks/ set to link.
# One list, appended wherever TranscriptionController.cpp appears, so a new
# kind is added in exactly one place here rather than N test targets.
set(SUOP_TASK_SOURCES
    tasks/Task.cpp tasks/TaskContext.cpp tasks/EngineCache.cpp tasks/TaskRunner.cpp tasks/TaskFactory.cpp
    tasks/DownloadTask.cpp tasks/TranscribeTask.cpp tasks/RelabelTask.cpp tasks/ChaptersTask.cpp
    tasks/DiarizeTask.cpp tasks/DetectSilenceTask.cpp tasks/RenameSpeakerTask.cpp tasks/MergeSpeakersTask.cpp tasks/CombineTask.cpp
    tasks/UncombineTask.cpp
    tasks/ImportTask.cpp tasks/UsbGrabTask.cpp tasks/BenchmarkTask.cpp tasks/DiarizationSweepTask.cpp
    tasks/RenameFileTask.cpp tasks/MoveTask.cpp tasks/ModelIoTask.cpp
    tasks/TrashTask.cpp tasks/DeleteTask.cpp tasks/RestoreTask.cpp
    tasks/FolderRenameTask.cpp tasks/FolderMoveTask.cpp FileRename.cpp VaultFileOps.cpp
    TranscribePipeline.cpp
    # The transcript .md file-format helpers (formatTimestamp/writeFrontmatter/
    # SegmentLineWriter/the body splices) split out of TranscriptionController.cpp
    # — every task kind that touches a sidecar calls them, so it belongs in the
    # same one list rather than N per-target additions.
    TranscriptFormat.cpp
    # The phase-timing / progress-estimation collaborator split out of
    # TranscriptionController.cpp — the controller holds one as a member, so
    # it's needed by exactly the same set of targets, hence the same one list.
    PhaseTimer.cpp
    # The split-recording combine DETECTION collaborator split out of
    # TranscriptionController.cpp — the controller's combine queue glue calls
    # it directly, so it's needed by exactly the same set of targets, hence
    # the same one list.
    CombineScanner.cpp
    # The job-list PRESENTATION collaborator (phase registry + the pure
    # collapse/sort display transforms) split out of
    # TranscriptionController.cpp — the controller forwards to it directly, so
    # it's needed by exactly the same set of targets, hence the same one list.
    JobListFormatter.cpp
    # The pure task-list QUERY/SERIALIZATION collaborator (indexOfTask/
    # isQueued/tasksTotal/frontOfQueueInsertIndex/the moveQueuedJob index math/
    # persistQueue's JSON) split out of TranscriptionController.cpp — the
    # controller forwards to it directly, so it's needed by exactly the same
    # set of targets, hence the same one list.
    TaskQuery.cpp
    # PHASE Q: the display-blind queue engine split out of
    # TranscriptionController.cpp — the controller holds one as a member and
    # implements its observer interface, so it's needed by exactly the same set
    # of targets, hence the same one list.
    TaskScheduler.cpp)

# Each test compiles the core sources it needs directly — no core static lib,
# so the app target stays untouched. QML_ELEMENT headers compile outside the
# qml module as long as Qt6::Qml is linked.
function(suop_add_test name)
    cmake_parse_arguments(ARG "LINK_WHISPER;LINK_LLAMA;LINK_SHERPA_ONNX;LINK_QUICK;LINK_MULTIMEDIA;LINK_DBUS;LINK_KWINDOWSYSTEM;LINK_GUI;LINK_SVG;ICON_ASSETS" "" "SOURCES" ${ARGN})
    set(abs_sources "")
    foreach(src ${ARG_SOURCES})
        list(APPEND abs_sources ${SUOP_CORE_DIR}/${src})
    endforeach()
    qt_add_executable(${name} ${name}.cpp ${abs_sources})
    target_include_directories(${name} PRIVATE ${SUOP_CORE_DIR})
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
    if(ARG_LINK_QUICK)
        # For tests that load real .qml files into an actual (offscreen)
        # QQuickView rather than just linking a QML_ELEMENT C++ class — see
        # tst_sidebar_keynav.cpp's top-of-file comment for why this is
        # needed and what it does/doesn't cover.
        target_link_libraries(${name} PRIVATE Qt6::Quick Qt6::QuickControls2)
    endif()
    if(ARG_LINK_DBUS)
        # For tests that compile UsbDeviceMonitor.cpp (QDBusConnection::systemBus()
        # usage) — either directly, or transitively via UsbGrabberController.cpp,
        # which constructs one. Needed even for tests that only exercise
        # usbDeviceIsExternalMedia(), a plain function over property maps: the TU
        # around it still references QtDBus and has to link.
        target_link_libraries(${name} PRIVATE Qt6::DBus)
    endif()
    if(ARG_LINK_KWINDOWSYSTEM)
        # For tests that compile DesktopIntegration.cpp — it mints xdg-activation
        # tokens via KWaylandExtras. Pulls in Qt6::Gui transitively (KF6's own
        # public dependency), which that TU also needs for QGuiApplication.
        target_link_libraries(${name} PRIVATE KF6::WindowSystem)
    endif()
    if(ARG_LINK_MULTIMEDIA)
        # For tests that compile AudioRecorder.cpp/AudioPlayer.cpp — those
        # reference QAudioSource/QMediaDevices etc. even when the test itself
        # only exercises hardware-free helper functions (see
        # tst_audiorecorder.cpp), so the TU still needs Qt6::Multimedia to link.
        target_link_libraries(${name} PRIVATE Qt6::Multimedia)
    endif()
    if(ARG_LINK_GUI)
        # For tests that compile a core source needing QtGui directly without
        # dragging in Quick — Clipboard.cpp (QGuiApplication::clipboard()).
        # Also what puts QT_GUI_LIB in the TU, so the test's own
        # QGuiApplication is available.
        target_link_libraries(${name} PRIVATE Qt6::Gui)
    endif()
    if(ARG_LINK_SVG)
        # For tests that compile IconTheme.cpp — IconImageProvider uses
        # QSvgRenderer to raster svg icon packs.
        target_link_libraries(${name} PRIVATE Qt6::Svg)
    endif()
    if(ARG_ICON_ASSETS)
        # IconTheme discovers packs by scanning
        # :/qt/qml/SuopTranscriber/assets/icons, a path the app's
        # qt_add_qml_module RESOURCES block normally provides. Tests don't
        # link that module (see this file's header comment), so mirror the
        # same files into the test binary at the SAME resource path — BASE
        # makes each alias relative to the repo root, so they land at
        # assets/icons/... under the prefix, exactly as in the app.
        # SUOP_ICON_ASSETS is the parent CMakeLists' glob, and is relative to
        # CMAKE_SOURCE_DIR.
        set(icon_abs "")
        foreach(rel ${SUOP_ICON_ASSETS})
            list(APPEND icon_abs "${CMAKE_SOURCE_DIR}/${rel}")
        endforeach()
        qt_add_resources(${name} "suop_icons_${name}"
            PREFIX "/qt/qml/SuopTranscriber"
            BASE "${CMAKE_SOURCE_DIR}"
            FILES ${icon_abs}
        )
    endif()
    if(ARG_LINK_WHISPER)
        target_link_libraries(${name} PRIVATE whisper)
    endif()
    # Only link llama when labeling is actually built (mirrors the app target).
    # With -DSUOP_ENABLE_LABELING=OFF the llama target doesn't exist and
    # LabelingEngine.cpp compiles its stub, so no test needs (or can) link it.
    if(ARG_LINK_LLAMA AND SUOP_ENABLE_LABELING)
        target_link_libraries(${name} PRIVATE llama)
    endif()
    if(ARG_LINK_SHERPA_ONNX)
        target_link_libraries(${name} PRIVATE sherpa-onnx-cxx-api)
        set_target_properties(${name} PROPERTIES BUILD_RPATH "$ORIGIN")
        if(SHERPA_ONNX_ENABLE_GPU)
            add_dependencies(${name} suop_copy_cuda12_compat_libs)
        endif()
    endif()
    target_compile_definitions(${name} PRIVATE
        SUOP_REPO_DIR="${CMAKE_SOURCE_DIR}"
        SUOP_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/third_party/whisper.cpp/samples"
        SUOP_QML_DIR="${CMAKE_SOURCE_DIR}/qml"
        SUOP_SRC_DIR="${CMAKE_SOURCE_DIR}/src"
        # Keep every target that compiles LabelingEngine.cpp on the same branch
        # as the app: ON links llama and builds the real engine, OFF builds the
        # stub. Harmless on tests that don't compile it.
        $<$<BOOL:${SUOP_ENABLE_LABELING}>:SUOP_ENABLE_LABELING>
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

suop_add_test(tst_smoke)
suop_add_test(tst_rollingaverage)
# Header-only helper (ProcessRunner.h) — no SOURCES, just needs SUOP_CORE_DIR
# on the include path (added for every test above) and a real child process.
suop_add_test(tst_processrunner)
suop_add_test(tst_vaulttreemodel SOURCES VaultTreeModel.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_desktopintegration LINK_DBUS LINK_KWINDOWSYSTEM SOURCES DesktopIntegration.cpp)
suop_add_test(tst_clipboard LINK_GUI SOURCES Clipboard.cpp)
# Loads the real BottomBar.qml in a QQuickView. Deliberately does NOT compile
# DesktopIntegration.cpp — it registers a recording stub under that name, so a
# clicked path segment never launches a real file manager (see the test's
# top-of-file comment).
suop_add_test(tst_bottombar_pathsegments LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# Loads the real BottomBar.qml (inside an ApplicationWindow — its Tasks popup
# needs a real Overlay, same reason tst_snackbar.cpp isn't a bare QQuickView)
# and proves each Tasks-list row renders which model it will run. Compiles the
# REAL ModelManager.cpp so JobListFormatter::modelShortLabel()'s catalog
# mapping is exercised end to end, not re-asserted against itself.
suop_add_test(tst_bottombar_taskmodel LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp JobListFormatter.cpp ModelManager.cpp)
# Loads the real BottomBar.qml (ApplicationWindow, same Overlay requirement as
# tst_bottombar_taskmodel above) and proves the Tasks-list per-row × button is
# actually clickable on its RIGHT edge — the overlay ScrollBar used to float
# on top of that lane and swallow the press. See its top-of-file comment.
suop_add_test(tst_bottombar_tasklist LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# Loads the real BottomBar.qml (inside an ApplicationWindow — its Tasks popup
# needs a real Overlay, same reason tst_snackbar.cpp isn't a bare QQuickView)
# and proves clicking the "Kind: filename" task label emits taskActivated for
# the displayed job's file (active + queued paths), and stays inert when
# there's no file or the setting is off.
suop_add_test(tst_bottombar_taskopen LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# Loads the real MainPanel.qml in a QQuickView and proves the chapter-title <->
# transcript cross-selection offset math (which the tst_styleguide source scans
# structurally cannot check). Compiles the REAL Clipboard.cpp — not a stub —
# because the assertion is what actually lands on the clipboard; hence LINK_GUI.
# See its top-of-file comment.
suop_add_test(tst_mainpanel_chapterselection LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp)
suop_add_test(tst_mainpanel_guttertimestamp LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp)
suop_add_test(tst_scratch_headingrepro LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp)
# Loads the real BenchmarkPage.qml inside a modal Popup -> ScrollView (its
# SettingsPopup nesting) and proves the benchmark file follows the left panel's
# currentFilePath (the in-page file-picker ComboBox was removed).
suop_add_test(tst_benchmarkpage_filesync LINK_QUICK SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp FileRename.cpp VaultFileOps.cpp)
# Source-scan guard for the Benchmark page's LIVE transcript streaming (item
# 066): the running card is created as a `running: true` placeholder on start,
# grown by onBenchmarkModelProgressText as segments stream in, and finalized in
# place on done (not append-at-done). No QQuickView can drive the Connections
# handlers here (their controller is null in the filesync harness), so this
# scans qml/BenchmarkPage.qml per the tst_styleguide fallback pattern.
suop_add_test(tst_benchmarkpage_livestream)
# Loads the real MainPanel.qml in a QQuickView AND wires a REAL
# TranscriptionController into it — the assertion is what the panel reads back
# off disk for the file that's actually open, so a stubbed readChapters() would
# only prove the stub agrees with itself. Hence the whisper/llama link deps that
# the other MainPanel QML tests don't need. See its top-of-file comment.
suop_add_test(tst_mainpanel_chapterrefresh LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI
    LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp
            ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp
            UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
# Loads the real MainPanel.qml + a REAL TranscriptionController and drives the
# queue (fake zero-byte model → fast-fail jobs) to prove busyOrQueuedUrls clears
# reactively when a job finishes — the per-file action buttons' re-enable. Same
# link deps as the other real-controller MainPanel harness. See its top comment.
suop_add_test(tst_mainpanel_buttonstate LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI
    LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp
            ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp
            UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
# Loads the real MainPanel.qml and proves the live transcript follows the tail
# as content streams in — including a single large (>threshold) growth, the case
# that regressed. Drives the follow deterministically (no model) plus one real
# whisper job (model-gated). Same link deps as the other real-controller
# MainPanel harnesses. See its top-of-file comment.
suop_add_test(tst_mainpanel_livescroll LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI
    LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp
            ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp
            UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_mainpanel_spacedload LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI
    LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp
            ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp
            UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_mainpanel_deleteundo LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI
    LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp VaultTreeModel.cpp TranscriptionController.cpp
            WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp
            UsbGrabJob.cpp UsbRuleStore.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_mainpanel_renameundo LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI
    LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp VaultTreeModel.cpp TranscriptionController.cpp
            WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp
            UsbGrabJob.cpp UsbRuleStore.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_mainpanel_exittoplevel LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp)
# Queue item 072: loads the REAL MainPanel.qml and installs the PRODUCTION
# FocusBlurFilter on qApp to prove the app-wide "click outside blurs the focused
# field" survives a press swallowed by the transcript ScrollView — the exact gap
# a source scan can't see. Same real-controller dep set as the other MainPanel
# harnesses, plus FocusBlurFilter.cpp (the thing under test).
suop_add_test(tst_focusblurfilter LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI
    LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES IconTheme.cpp Clipboard.cpp TranscriptDisplayBuilder.cpp UndoStack.cpp FocusBlurFilter.cpp VaultTreeModel.cpp TranscriptionController.cpp
            WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp
            UsbGrabJob.cpp UsbRuleStore.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_vaultsearchproxymodel SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_modelmanager SOURCES ModelManager.cpp)
# Characterization test for the transcript display-model builder lifted out of
# MainPanel.qml's displayData binding — a pure data-in/data-out transform, so
# it needs no QQuickView (and deliberately doesn't use one: the point is to
# pin the assembly rules themselves, independent of any QML wiring).
suop_add_test(tst_transcript_display_builder SOURCES TranscriptDisplayBuilder.cpp)
# Characterization test for the file-tree ordering computation lifted out of
# Sidebar.qml's refreshTreeOrdering(). Pure data-in/data-out, so no QQuickView
# — it pins the PRODUCER half of the fileSortKeys contract, with
# tst_vaultsearchproxymodel covering the CONSUMER half (lessThan()).
suop_add_test(tst_tree_ordering SOURCES TreeOrdering.cpp)
suop_add_test(tst_transcript_format LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp
            UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_transcription_queue LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp
            UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_filecombine LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp
            UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_engine_contract LINK_WHISPER
    SOURCES WhisperCppEngine.cpp AudioDecoder.cpp)
suop_add_test(tst_parakeetengine LINK_SHERPA_ONNX SOURCES ParakeetEngine.cpp AudioDecoder.cpp)
suop_add_test(tst_enginemanifest SOURCES EngineManifest.cpp)
# Pilot for the task-system refactor (.task/plan.md) — validates Task/
# TaskContext/TaskRunner/EngineCache generically (FakeTask) plus DownloadTask's
# static interface, without touching TranscriptionController (that rewrite is
# a later phase). LabelingEngine.cpp/DiarizationEngine.cpp are only here so
# EngineCache's destructor links (see EngineCache.cpp's doc comment) — neither
# is actually exercised since ensureAsr()/ensureLabel() aren't implemented yet.
# EngineCache.cpp is ONE translation unit — now that it also implements
# ensureAsr() (for TranscribeTask/BenchmarkTask), every test that compiles it
# needs the FULL engine dependency set to link, even a download-only test
# that never calls ensureAsr() at runtime.
suop_add_test(tst_task_download LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
# RenameFileTask itself is pure filesystem, but the test constructs a
# TaskContext (which references an EngineCache whose out-of-line ctor pulls in
# the engine chain), so it needs the same full dep set as the other task tests.
suop_add_test(tst_task_renamefile LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
# MoveTask: same story as tst_task_renamefile — pure filesystem run(), but the
# test builds a TaskContext (EngineCache ctor pulls the engine chain), so it
# carries the same full dependency set.
suop_add_test(tst_task_move LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
# TrashTask/DeleteTask/RestoreTask: pure filesystem run() over VaultFileOps, but
# the test builds a TaskContext (EngineCache), so it carries the same full
# dependency set — plus VaultTreeModel.cpp, which TranscriptionController.cpp now
# links (applyRemovalOutcome/setVaultTreeModel).
suop_add_test(tst_task_trash LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
# FolderRenameTask/FolderMoveTask: same dependency set (EngineCache pulls the
# engine chain + TranscriptionController, which links VaultTreeModel.cpp).
suop_add_test(tst_task_folder LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
# RenameSpeakerTask/MergeSpeakersTask call the shared transcript-format
# helpers TranscriptionController.h now exposes (renameSpeakerInFile() etc,
# defined in TranscriptionController.cpp) — same full dependency set as
# tst_transcription_queue since it's all one translation unit.
suop_add_test(tst_task_speaker_edits LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
# CombineTask/ImportTask/UsbGrabTask are self-contained (no shared
# transcript-format helper, no TranscriptionController.cpp dependency) —
# just QProcess/QFile plumbing plus UsbGrabJob.cpp's runUsbGrabScan().
# LabelingEngine.cpp/DiarizationEngine.cpp are only here so EngineCache's
# destructor links (see EngineCache.cpp's doc comment), same as
# tst_task_download.
# Same full-dependency-set requirement as tst_task_download, above.
suop_add_test(tst_task_files LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
# RelabelTask calls shared transcript-format helpers from
# TranscriptionController.h (sidecarPath/parseSpeakersList/
# plainTextFromTranscriptBody/collectVaultTags/writeLabelingFields/
# kPreludeEndMarker) — same full dependency set as tst_task_speaker_edits.
suop_add_test(tst_task_relabel LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_task_chapters LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_task_diarize LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
# DetectSilenceTask calls the shared transcript-format helpers
# (sidecarPath/parsedSegmentsFromTranscriptBody/parseSpeakersList/
# formatSilencesValue) TranscriptionController.h exposes — same full
# dependency set as tst_task_diarize.
suop_add_test(tst_task_silence LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_task_transcribe LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_task_benchmark LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp
            ${SUOP_TASK_SOURCES})
suop_add_test(tst_subprocessengine SOURCES SubprocessEngine.cpp EngineManifest.cpp)
suop_add_test(tst_engineregistry SOURCES EngineRegistry.cpp EngineManifest.cpp GpuProbe.cpp)
suop_add_test(tst_vadchunking LINK_WHISPER SOURCES VadChunker.cpp)
suop_add_test(tst_transcribe_pipeline LINK_WHISPER
    SOURCES TranscribePipeline.cpp VadChunker.cpp ModelManager.cpp WhisperCppEngine.cpp)
suop_add_test(tst_audiodecoder SOURCES AudioDecoder.cpp)
suop_add_test(tst_audiorecorder LINK_MULTIMEDIA SOURCES AudioRecorder.cpp)
suop_add_test(tst_labelingengine LINK_LLAMA SOURCES LabelingEngine.cpp)
suop_add_test(tst_diarizationengine LINK_SHERPA_ONNX SOURCES DiarizationEngine.cpp AudioDecoder.cpp)
suop_add_test(tst_future_spec)
suop_add_test(tst_styleguide)
suop_add_test(tst_benchmark_combos)
suop_add_test(tst_icontheme LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# OPT-IN pixel-snapshot harness — built so it never bit-rots, but DORMANT unless
# SUOP_SNAPSHOT=update|compare is set (it QSKIPs otherwise), so a normal `ctest`
# run does nothing here. Goldens are environment-specific and NOT committed (see
# .gitignore's tests/snapshots/). Renders real QML (AppIcon) offscreen, hence
# LINK_QUICK + icon resources like the other QQuickView harnesses.
suop_add_test(tst_visual_snapshot LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# Renders the real Snackbar/SnackbarHost/Notifications in an ApplicationWindow
# (not a QQuickView — SnackbarHost needs Overlay.overlay; see the test's
# top-of-file comment). Needs IconTheme + icon resources for the close AppIcon.
suop_add_test(tst_snackbar LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# MiddleDragScroll.qml's own harness: pure QML (the component imports nothing
# from this module and needs no C++ types), so it links Quick but compiles no
# core sources.
suop_add_test(tst_middledragscroll LINK_QUICK)
# TextRedoShortcut.qml's harness: a QQuickView driver with a real
# TextField/TextArea, so the Ctrl+Y redo chord is proven against actual Qt
# text-editing behavior. No app C++ and no icons — the component imports
# nothing from the module.
suop_add_test(tst_textredoshortcut LINK_QUICK)
# ModelComboBox.qml's own harness: a QQuickView driver assigning its model
# after completion, the way MainPanel does — proves the real
# QQuickComboBox::setModel() reset behavior rather than guessing at it. No
# app C++ and no icons (the component never touches AppIcon/IconTheme).
suop_add_test(tst_modelpicker LINK_QUICK)
# RecorderPage.qml's runtime harness — the recorder was the one screen with no
# QQuickView coverage, guarded only by tst_styleguide source scans. Needs
# AudioRecorder.cpp (the stub delegates buildAppCaptureRows/decayPeak/… to the
# REAL AudioRecorderIO helpers, so row logic is under test rather than
# re-stated), hence LINK_MULTIMEDIA; and IconTheme + icon assets because the
# page uses AppIcon.
suop_add_test(tst_recorderpage_sources LINK_QUICK LINK_MULTIMEDIA LINK_SVG ICON_ASSETS
    SOURCES AudioRecorder.cpp IconTheme.cpp)
# TitleBar.qml's own harness: loads the real TitleBar (frameless-mode custom
# titlebar), so like the sidebar harnesses below it needs IconTheme (its
# singleton + image provider) and the icon-pack resources for AppIcon.
suop_add_test(tst_titlebar_buttons LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# The sidebar QQuickView harnesses load the real Sidebar.qml, which now uses
# AppIcon — so they need IconTheme (its singleton + image provider) and the
# icon-pack resources, exactly as the app does, or AppIcon.qml throws
# "IconTheme is not defined" at load.
suop_add_test(tst_sidebar_keynav LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp TreeOrdering.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_sidebar_dragmove LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp TreeOrdering.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_sidebar_searchscroll LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp TreeOrdering.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_sidebar_rename LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp TreeOrdering.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_sidebar_expand LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp TreeOrdering.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_sidebar_scroll LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp TreeOrdering.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_sidebar_guides LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp TreeOrdering.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_sidebar_selectall LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp TreeOrdering.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_sidebar_filtersuggestions LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp TreeOrdering.cpp FileRename.cpp VaultFileOps.cpp)
# The ONLY sidebar harness that renders inside a real ApplicationWindow (hence a
# real Overlay + SnackbarHost), which is what the context menu's modal dimming
# needs — the QQuickView harnesses above structurally cannot show it. See its
# top-of-file comment.
suop_add_test(tst_sidebar_contextmenu LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp TreeOrdering.cpp FileRename.cpp VaultFileOps.cpp)
suop_add_test(tst_usbdevicemonitor LINK_DBUS SOURCES UsbDeviceMonitor.cpp)
suop_add_test(tst_usbrulestore SOURCES UsbRuleStore.cpp)
suop_add_test(tst_usbgrabjob SOURCES UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp FileRename.cpp VaultFileOps.cpp)
# UsbGrabberController now enqueues every grab onto TranscriptionController's
# task queue (see its class doc comment), so these two drag in the controller
# and its whole engine dependency chain — the same set the other real-controller
# tests above link. Not a stub: the queue IS the thing under test here (a grab
# has to actually become a job), and a stubbed one would only agree with itself.
suop_add_test(tst_usbgrabbercontroller LINK_DBUS LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES UsbGrabberController.cpp UsbDeviceMonitor.cpp UsbRuleStore.cpp UsbGrabJob.cpp VaultTreeModel.cpp
            TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp
            ${SUOP_TASK_SOURCES})
# Loads the real UsbRegexField.qml in a QQuickView — tst_styleguide.cpp keeps
# the cheap source-scan guard, this one proves the component actually LOADS
# (a green cmake build doesn't) and that its validity indicator/token buttons
# behave. See its top-of-file comment.
suop_add_test(tst_usbregexfield LINK_QUICK LINK_DBUS LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES UsbGrabberController.cpp UsbDeviceMonitor.cpp UsbRuleStore.cpp UsbGrabJob.cpp VaultTreeModel.cpp
            TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp
            EngineRegistry.cpp ParakeetEngine.cpp
            ${SUOP_TASK_SOURCES})
