# Test suite = living spec sheet. Each tst_* documents expected behaviors from
# DESIGN.md / CLAUDE.md / StyleGuide.md. Documented-but-unbuilt behaviors live
# in tst_future_spec as QSKIP placeholders so every ctest run lists them.

find_package(Qt6 6.7 REQUIRED COMPONENTS Test)

set(SUOP_CORE_DIR ${CMAKE_SOURCE_DIR}/src/core)

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
    if(ARG_LINK_LLAMA)
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
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

suop_add_test(tst_smoke)
suop_add_test(tst_rollingaverage)
suop_add_test(tst_vaulttreemodel SOURCES VaultTreeModel.cpp)
suop_add_test(tst_desktopintegration LINK_DBUS LINK_KWINDOWSYSTEM SOURCES DesktopIntegration.cpp)
suop_add_test(tst_clipboard LINK_GUI SOURCES Clipboard.cpp)
# Loads the real BottomBar.qml in a QQuickView. Deliberately does NOT compile
# DesktopIntegration.cpp — it registers a recording stub under that name, so a
# clicked path segment never launches a real file manager (see the test's
# top-of-file comment).
suop_add_test(tst_bottombar_pathsegments LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# Loads the real MainPanel.qml in a QQuickView and proves the chapter-title <->
# transcript cross-selection offset math (which the tst_styleguide source scans
# structurally cannot check). Compiles the REAL Clipboard.cpp — not a stub —
# because the assertion is what actually lands on the clipboard; hence LINK_GUI.
# See its top-of-file comment.
suop_add_test(tst_mainpanel_chapterselection LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI SOURCES IconTheme.cpp Clipboard.cpp)
suop_add_test(tst_scratch_headingrepro LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI SOURCES IconTheme.cpp Clipboard.cpp)
# Loads the real BenchmarkPage.qml inside a modal Popup -> ScrollView (its
# SettingsPopup nesting) and proves the audio-file ComboBox is populated +
# selectable — the allAudioPaths() no-NOTIFY binding trap that left it empty.
suop_add_test(tst_benchmarkpage_filecombo LINK_QUICK SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp)
# Loads the real MainPanel.qml in a QQuickView AND wires a REAL
# TranscriptionController into it — the assertion is what the panel reads back
# off disk for the file that's actually open, so a stubbed readChapters() would
# only prove the stub agrees with itself. Hence the whisper/llama link deps that
# the other MainPanel QML tests don't need. See its top-of-file comment.
suop_add_test(tst_mainpanel_chapterrefresh LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI
    LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES IconTheme.cpp Clipboard.cpp TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp
            ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp)
# Loads the real MainPanel.qml + a REAL TranscriptionController and drives the
# queue (fake zero-byte model → fast-fail jobs) to prove busyOrQueuedUrls clears
# reactively when a job finishes — the per-file action buttons' re-enable. Same
# link deps as the other real-controller MainPanel harness. See its top comment.
suop_add_test(tst_mainpanel_buttonstate LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI
    LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES IconTheme.cpp Clipboard.cpp TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp
            ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp)
suop_add_test(tst_mainpanel_deleteundo LINK_QUICK LINK_SVG ICON_ASSETS LINK_GUI
    LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES IconTheme.cpp Clipboard.cpp VaultTreeModel.cpp TranscriptionController.cpp
            WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp
            LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp)
suop_add_test(tst_vaultsearchproxymodel SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp)
suop_add_test(tst_modelmanager SOURCES ModelManager.cpp)
suop_add_test(tst_transcript_format LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp)
suop_add_test(tst_transcription_queue LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp)
suop_add_test(tst_filecombine LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp VadChunker.cpp ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp
            EngineManifest.cpp GpuProbe.cpp SubprocessEngine.cpp EngineRegistry.cpp ParakeetEngine.cpp)
suop_add_test(tst_engine_contract LINK_WHISPER
    SOURCES WhisperCppEngine.cpp AudioDecoder.cpp)
suop_add_test(tst_parakeetengine LINK_SHERPA_ONNX SOURCES ParakeetEngine.cpp AudioDecoder.cpp)
suop_add_test(tst_enginemanifest SOURCES EngineManifest.cpp)
suop_add_test(tst_subprocessengine SOURCES SubprocessEngine.cpp EngineManifest.cpp)
suop_add_test(tst_engineregistry SOURCES EngineRegistry.cpp EngineManifest.cpp GpuProbe.cpp)
suop_add_test(tst_vadchunking LINK_WHISPER SOURCES VadChunker.cpp)
suop_add_test(tst_audiodecoder SOURCES AudioDecoder.cpp)
suop_add_test(tst_audiorecorder LINK_MULTIMEDIA SOURCES AudioRecorder.cpp)
suop_add_test(tst_labelingengine LINK_LLAMA SOURCES LabelingEngine.cpp)
suop_add_test(tst_diarizationengine LINK_SHERPA_ONNX SOURCES DiarizationEngine.cpp AudioDecoder.cpp)
suop_add_test(tst_future_spec)
suop_add_test(tst_styleguide)
suop_add_test(tst_icontheme LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# Renders the real Snackbar/SnackbarHost/Notifications in an ApplicationWindow
# (not a QQuickView — SnackbarHost needs Overlay.overlay; see the test's
# top-of-file comment). Needs IconTheme + icon resources for the close AppIcon.
suop_add_test(tst_snackbar LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# MiddleDragScroll.qml's own harness: pure QML (the component imports nothing
# from this module and needs no C++ types), so it links Quick but compiles no
# core sources.
suop_add_test(tst_middledragscroll LINK_QUICK)
# TitleBar.qml's own harness: loads the real TitleBar (frameless-mode custom
# titlebar), so like the sidebar harnesses below it needs IconTheme (its
# singleton + image provider) and the icon-pack resources for AppIcon.
suop_add_test(tst_titlebar_buttons LINK_QUICK LINK_SVG ICON_ASSETS SOURCES IconTheme.cpp)
# The sidebar QQuickView harnesses load the real Sidebar.qml, which now uses
# AppIcon — so they need IconTheme (its singleton + image provider) and the
# icon-pack resources, exactly as the app does, or AppIcon.qml throws
# "IconTheme is not defined" at load.
suop_add_test(tst_sidebar_keynav LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp)
suop_add_test(tst_sidebar_dragmove LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp)
suop_add_test(tst_sidebar_rename LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp)
suop_add_test(tst_sidebar_expand LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp)
suop_add_test(tst_sidebar_guides LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp)
# The ONLY sidebar harness that renders inside a real ApplicationWindow (hence a
# real Overlay + SnackbarHost), which is what the context menu's modal dimming
# needs — the QQuickView harnesses above structurally cannot show it. See its
# top-of-file comment.
suop_add_test(tst_sidebar_contextmenu LINK_QUICK LINK_SVG ICON_ASSETS SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp IconTheme.cpp)
suop_add_test(tst_usbdevicemonitor LINK_DBUS SOURCES UsbDeviceMonitor.cpp)
suop_add_test(tst_usbrulestore SOURCES UsbRuleStore.cpp)
suop_add_test(tst_usbgrabjob SOURCES UsbGrabJob.cpp UsbRuleStore.cpp VaultTreeModel.cpp)
suop_add_test(tst_usbgrabbercontroller LINK_DBUS
    SOURCES UsbGrabberController.cpp UsbDeviceMonitor.cpp UsbRuleStore.cpp UsbGrabJob.cpp VaultTreeModel.cpp)
# Loads the real UsbRegexField.qml in a QQuickView — tst_styleguide.cpp keeps
# the cheap source-scan guard, this one proves the component actually LOADS
# (a green cmake build doesn't) and that its validity indicator/token buttons
# behave. See its top-of-file comment.
suop_add_test(tst_usbregexfield LINK_QUICK LINK_DBUS
    SOURCES UsbGrabberController.cpp UsbDeviceMonitor.cpp UsbRuleStore.cpp UsbGrabJob.cpp VaultTreeModel.cpp)
