cmake_minimum_required(VERSION 3.21)
project(suopTranscriber VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(SUOP_ENABLE_VULKAN "Build whisper.cpp with Vulkan backend (cross-vendor GPU baseline)" ON)
option(SUOP_ENABLE_CUDA "Build whisper.cpp with CUDA backend (NVIDIA fast path)" OFF)
option(SUOP_ENABLE_LABELING "Build with llama.cpp for AI auto-labeling" ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Multimedia Core5Compat DBus Svg)
# 6.19 for KWaylandExtras::xdgActivationToken()'s QFuture overload, which
# DesktopIntegration needs to raise the file manager on Wayland.
find_package(KF6WindowSystem 6.19 REQUIRED)

qt_standard_project_setup(REQUIRES 6.7)

# whisper.cpp is linked in-process (see DESIGN.md); GGML_VULKAN is the
# cross-vendor floor, GGML_CUDA is an opt-in NVIDIA fast path.
set(GGML_VULKAN ${SUOP_ENABLE_VULKAN} CACHE BOOL "" FORCE)
set(GGML_CUDA ${SUOP_ENABLE_CUDA} CACHE BOOL "" FORCE)

# llama.cpp (AI auto-labeling, see DESIGN.md "Model management"). Vendored
# BEFORE whisper.cpp so the shared `ggml` target comes from llama.cpp's copy:
# both projects guard with `if (NOT TARGET ggml)` and reuse an existing
# target, and llama.cpp tracks ggml more closely (whisper.cpp's periodic ggml
# sync lags, so building llama.cpp against whisper's older ggml breaks on new
# ggml ops; the additive ggml API means the older whisper.cpp compiles fine
# against the newer copy).
if(SUOP_ENABLE_LABELING)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/llama.cpp)
endif()

set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/whisper.cpp)

# sherpa-onnx (speaker diarization — pyannote segmentation + CAM++ embedding
# pipeline, see DESIGN.md "Speaker diarization"). Vendored whole rather than
# hand-rolled against raw ONNX Runtime: its C++ API already implements the
# exact segmentation-powerset decode + embedding + agglomerative-clustering
# pipeline, tested and correct — reimplementing that preprocessing by hand
# turned out to be a real correctness trap (subtle Fbank/normalization
# mismatches silently produce garbage embeddings), not worth the smaller
# build size. Pulls in more than we use (full ASR/VAD/TTS/openfst — no way to
# scope its CMake down further) but sidesteps that risk entirely. It fetches
# its own prebuilt ONNX Runtime internally. ONNX Runtime has no Vulkan EP
# (unlike whisper.cpp's cross-vendor baseline), so GPU here means CUDA only
# — NVIDIA-only, requires the CUDA toolkit + cuDNN installed system-wide
# (unlike whisper.cpp's Vulkan path, which just needs the driver). Enabling
# this also forces sherpa-onnx's own libs to build as SHARED (its own
# CMakeLists requires BUILD_SHARED_LIBS=ON for the GPU onnxruntime package).
# DiarizationEngine falls back to CPU automatically if CUDA init fails at
# runtime (see its own comments) — this option controls whether the CUDA
# code path exists in the binary at all, not which one actually runs.
set(SHERPA_ONNX_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_PORTAUDIO OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_JNI OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_GPU ON CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_DIRECTML OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_WASM OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_BUILD_C_API_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_TTS OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION ON CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_C_API ON CACHE BOOL "" FORCE)

# Local patch to the vendored sherpa-onnx submodule (see the patch file's
# own header comment for what/why) — kept as a diff applied at configure
# time rather than committed inside the submodule itself, so the submodule
# stays pinned to the real upstream v1.13.4 tag and a fresh clone (or CI)
# stays fully reproducible via plain `git submodule update --init` with no
# extra remotes to manage. Idempotent: `git apply --reverse --check` tells
# us whether the patch's changes are already present in the working tree
# (exit 0) before deciding whether to (re-)apply it, so repeated
# configures — and a submodule that was already patched by a previous
# configure — don't fail with "patch does not apply".
set(SUOP_SHERPA_ONNX_PATCH ${CMAKE_SOURCE_DIR}/patches/sherpa-onnx-batch-segmentation.patch)
execute_process(
    COMMAND git apply --reverse --check ${SUOP_SHERPA_ONNX_PATCH}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx
    RESULT_VARIABLE SUOP_SHERPA_ONNX_PATCH_ALREADY_APPLIED
    OUTPUT_QUIET ERROR_QUIET
)
if(NOT SUOP_SHERPA_ONNX_PATCH_ALREADY_APPLIED EQUAL 0)
    message(STATUS "Applying local patch: sherpa-onnx-batch-segmentation.patch")
    execute_process(
        COMMAND git apply ${SUOP_SHERPA_ONNX_PATCH}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx
        RESULT_VARIABLE SUOP_SHERPA_ONNX_PATCH_APPLY_RESULT
    )
    if(NOT SUOP_SHERPA_ONNX_PATCH_APPLY_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply patches/sherpa-onnx-batch-segmentation.patch to "
            "third_party/sherpa-onnx. It's pinned against v1.13.4 — if the "
            "submodule was bumped past that, the patch needs re-deriving "
            "against the new source rather than force-applying.")
    endif()
else()
    message(STATUS "sherpa-onnx-batch-segmentation.patch already applied")
endif()

add_subdirectory(third_party/sherpa-onnx)

if(SHERPA_ONNX_ENABLE_GPU)
    # sherpa-onnx's fetched onnxruntime GPU build links against CUDA 12
    # specifically — confirmed via `ldd` on the actual
    # libonnxruntime_providers_cuda.so: it needs libcublasLt.so.12,
    # libcublas.so.12, libcufft.so.11, libcudart.so.12, libnvrtc.so.12 (exact
    # SONAMEs). A newer system CUDA install (e.g. this machine's CUDA 13, the
    # current version in Arch/CachyOS's `cuda` package as of this writing)
    # only provides the .so.13/.so.14 versions of these — NOT drop-in
    # compatible, so the CUDA execution provider fails to dlopen at runtime
    # and DiarizationEngine falls back to CPU (silently correct, per its own
    # comments, but leaves the GPU unused). Rather than requiring a SECOND
    # full system CUDA 12 toolkit install alongside CUDA 13, fetch just
    # these 4 redistributable libraries — the same PyPI wheels
    # PyTorch/TensorFlow bundle to run CUDA code without a system toolkit —
    # and copy their .so payloads next to sherpa-onnx's other shared libs
    # (already on suopTranscriber's RPATH). Pinned exact versions/hashes so
    # this is reproducible from a fresh checkout, not dependent on whatever
    # happens to be pip-installed on a given machine.
    include(FetchContent)
    # A macro, not a function: FetchContent_MakeAvailable() sets ${name}_SOURCE_DIR
    # in the CURRENT scope, and a function() body is its own scope — the
    # variable would be discarded on return instead of reaching the
    # add_custom_target() call below that needs it.
    macro(suop_fetch_cuda12_wheel name url sha256)
        FetchContent_Declare(${name}
            URL ${url}
            URL_HASH SHA256=${sha256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(${name})
    endmacro()

    suop_fetch_cuda12_wheel(nvidia_cublas_cu12
        "https://files.pythonhosted.org/packages/cb/c0/0a517bfe63ccd3b92eb254d264e28fca3c7cab75d07daea315250fb1bf73/nvidia_cublas_cu12-12.9.2.10-py3-none-manylinux_2_27_x86_64.whl"
        "e4f53a8ca8c5d6e8c492d0d0a3d565ecb59a751b19cfdaa4f6da0ab2104c1702")
    suop_fetch_cuda12_wheel(nvidia_cuda_runtime_cu12
        "https://files.pythonhosted.org/packages/bc/46/a92db19b8309581092a3add7e6fceb4c301a3fd233969856a8cbf042cd3c/nvidia_cuda_runtime_cu12-12.9.79-py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64.whl"
        "25bba2dfb01d48a9b59ca474a1ac43c6ebf7011f1b0b8cc44f54eb6ac48a96c3")
    suop_fetch_cuda12_wheel(nvidia_cufft_cu12
        "https://files.pythonhosted.org/packages/95/f4/61e6996dd20481ee834f57a8e9dca28b1869366a135e0d42e2aa8493bdd4/nvidia_cufft_cu12-11.4.1.4-py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64.whl"
        "c67884f2a7d276b4b80eb56a79322a95df592ae5e765cf1243693365ccab4e28")
    suop_fetch_cuda12_wheel(nvidia_cuda_nvrtc_cu12
        "https://files.pythonhosted.org/packages/b8/85/e4af82cc9202023862090bfca4ea827d533329e925c758f0cde964cb54b7/nvidia_cuda_nvrtc_cu12-12.9.86-py3-none-manylinux2010_x86_64.manylinux_2_12_x86_64.whl"
        "210cf05005a447e29214e9ce50851e83fc5f4358df8b453155d5e1918094dcb4")

    add_custom_target(suop_copy_cuda12_compat_libs ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/lib"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${nvidia_cublas_cu12_SOURCE_DIR}/nvidia/cublas/lib/libcublasLt.so.12"
            "${nvidia_cublas_cu12_SOURCE_DIR}/nvidia/cublas/lib/libcublas.so.12"
            "${nvidia_cuda_runtime_cu12_SOURCE_DIR}/nvidia/cuda_runtime/lib/libcudart.so.12"
            "${nvidia_cufft_cu12_SOURCE_DIR}/nvidia/cufft/lib/libcufft.so.11"
            "${nvidia_cuda_nvrtc_cu12_SOURCE_DIR}/nvidia/cuda_nvrtc/lib/libnvrtc.so.12"
            "${CMAKE_BINARY_DIR}/lib/"
        COMMENT "Copying CUDA 12 compat libraries (cuBLAS/cuFFT/cudart/nvrtc) for onnxruntime's CUDA execution provider"
    )
endif()

qt_add_executable(suopTranscriber
    src/main.cpp
)

# qt_add_qml_module only detects a QML-defined singleton via this source
# property — it does not scan file contents for `pragma Singleton`.
set_source_files_properties(qml/AppSettings.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/Notifications.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# Icon packs (see DESIGN.md "Iconography"): each assets/icons/<id>/ directory
# holds a pack.json manifest plus, for svg packs, the SVGs it names.
# IconTheme DISCOVERS packs by scanning this tree in the resource system
# rather than reading a hardcoded list, so globbing here (rather than naming
# 40+ files) keeps "a pack is just a directory" true on the build side too.
# CONFIGURE_DEPENDS re-runs the glob on rebuild, so adding an icon doesn't
# need a manual cmake re-run. The vendored LICENSE is deliberately not
# globbed in — it must ship in the repo beside the SVGs it covers, but
# nothing reads it at runtime.
#
# RELATIVE is load-bearing, not tidiness: qt_add_qml_module's RESOURCES uses
# each path verbatim as the resource ALIAS and hard-errors on an absolute one
# ("Please set the QT_RESOURCE_ALIAS property..."). Relative-to-source-dir
# paths are what make the aliases come out as assets/icons/<pack>/... under
# the module's /qt/qml/SuopTranscriber/ prefix — i.e. exactly IconTheme.cpp's
# kIconRoot.
file(GLOB_RECURSE SUOP_ICON_ASSETS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/*.svg"
)

qt_add_qml_module(suopTranscriber
    URI SuopTranscriber
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/AppSettings.qml
        qml/Notifications.qml
        qml/Snackbar.qml
        qml/SnackbarHost.qml
        qml/HoverBorder.qml
        qml/CombineGroupOutline.qml
        qml/UnderlineField.qml
        qml/UsbRegexField.qml
        qml/AppIcon.qml
        qml/PlayGlyph.qml
        qml/PauseGlyph.qml
        qml/TitleBar.qml
        qml/Sidebar.qml
        qml/MainPanel.qml
        qml/BottomBar.qml
        qml/SettingsPopup.qml
        qml/BenchmarkPage.qml
        qml/ModelsPage.qml
        qml/ModelComboBox.qml
        qml/MiddleDragScroll.qml
    RESOURCES
        ${SUOP_ICON_ASSETS}
    SOURCES
        src/core/IconTheme.h
        src/core/IconTheme.cpp
        src/core/Segment.h
        src/core/RollingAverage.h
        src/core/RegexValidation.h
        src/core/TranscriptionEngine.h
        src/core/VaultTreeModel.h
        src/core/VaultTreeModel.cpp
        src/core/VaultSearchProxyModel.h
        src/core/VaultSearchProxyModel.cpp
        src/core/AudioPlayer.h
        src/core/AudioPlayer.cpp
        src/core/AudioRecorder.h
        src/core/AudioRecorder.cpp
        src/core/UsbDeviceMonitor.h
        src/core/UsbDeviceMonitor.cpp
        src/core/UsbRuleStore.h
        src/core/UsbRuleStore.cpp
        src/core/UsbGrabJob.h
        src/core/UsbGrabJob.cpp
        src/core/UsbGrabberController.h
        src/core/UsbGrabberController.cpp
        src/core/WindowEffects.h
        src/core/WindowEffects.cpp
        src/core/DesktopIntegration.h
        src/core/DesktopIntegration.cpp
        src/core/Clipboard.h
        src/core/Clipboard.cpp
        src/core/AudioDecoder.h
        src/core/AudioDecoder.cpp
        src/core/ProcessRunner.h
        src/core/WhisperCppEngine.h
        src/core/WhisperCppEngine.cpp
        src/core/ParakeetEngine.h
        src/core/ParakeetEngine.cpp
        src/core/EngineManifest.h
        src/core/EngineManifest.cpp
        src/core/GpuProbe.h
        src/core/GpuProbe.cpp
        src/core/SubprocessEngine.h
        src/core/SubprocessEngine.cpp
        src/core/EngineRegistry.h
        src/core/EngineRegistry.cpp
        src/core/VadChunker.h
        src/core/VadChunker.cpp
        src/core/LabelingEngine.h
        src/core/LabelingEngine.cpp
        src/core/DiarizationEngine.h
        src/core/DiarizationEngine.cpp
        src/core/ModelManager.h
        src/core/ModelManager.cpp
        src/core/TranscriptionController.h
        src/core/TranscriptionController.cpp
        src/core/tasks/TaskControl.h
        src/core/tasks/ModelDownloadTarget.h
        src/core/tasks/TaskContext.h
        src/core/tasks/TaskContext.cpp
        src/core/tasks/TaskFactory.h
        src/core/tasks/TaskFactory.cpp
        src/core/tasks/EngineCache.h
        src/core/tasks/EngineCache.cpp
        src/core/tasks/Task.h
        src/core/tasks/Task.cpp
        src/core/tasks/TaskRunner.h
        src/core/tasks/TaskRunner.cpp
        src/core/tasks/DownloadTask.h
        src/core/tasks/DownloadTask.cpp
        src/core/tasks/RenameSpeakerTask.h
        src/core/tasks/RenameSpeakerTask.cpp
        src/core/tasks/MergeSpeakersTask.h
        src/core/tasks/MergeSpeakersTask.cpp
        src/core/tasks/CombineTask.h
        src/core/tasks/CombineTask.cpp
        src/core/tasks/ImportTask.h
        src/core/tasks/ImportTask.cpp
        src/core/tasks/UsbGrabTask.h
        src/core/tasks/UsbGrabTask.cpp
        src/core/tasks/RelabelTask.h
        src/core/tasks/RelabelTask.cpp
        src/core/tasks/ChaptersTask.h
        src/core/tasks/ChaptersTask.cpp
        src/core/tasks/DiarizeTask.h
        src/core/tasks/DiarizeTask.cpp
        src/core/tasks/TranscribeTask.h
        src/core/tasks/TranscribeTask.cpp
        src/core/tasks/BenchmarkTask.h
        src/core/tasks/BenchmarkTask.cpp
        src/core/tasks/DiarizationSweepTask.h
        src/core/tasks/DiarizationSweepTask.cpp
)

target_include_directories(suopTranscriber PRIVATE
    src/core
)

target_link_libraries(suopTranscriber PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Multimedia
    Qt6::Core5Compat
    Qt6::DBus
    # QSvgRenderer, for IconImageProvider's rastering of svg icon packs.
    Qt6::Svg
    KF6::WindowSystem
    whisper
    sherpa-onnx-cxx-api
)

# AI auto-labeling (llama.cpp) is opt-out: the llama target only exists when
# SUOP_ENABLE_LABELING is ON (see the add_subdirectory guard above), and
# LabelingEngine.cpp compiles a llama-free stub without this define. Linking
# llama unconditionally made -DSUOP_ENABLE_LABELING=OFF fail at generation.
if(SUOP_ENABLE_LABELING)
    target_link_libraries(suopTranscriber PRIVATE llama)
    target_compile_definitions(suopTranscriber PRIVATE SUOP_ENABLE_LABELING)
endif()

set_target_properties(suopTranscriber PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

if(SHERPA_ONNX_ENABLE_GPU)
    add_dependencies(suopTranscriber suop_copy_cuda12_compat_libs)
endif()

# sherpa-onnx's own cmake/eigen.cmake force-sets BUILD_TESTING OFF (CACHE
# ... FORCE) while vendoring Eigen, to keep Eigen's own test suite out of the
# build — that FORCE wins over anything set before add_subdirectory(
# third_party/sherpa-onnx) above, so it has to be forced back ON here,
# AFTER all vendored subdirectories, for this project's own tests/ to be
# configured by default.
set(BUILD_TESTING ON CACHE BOOL "" FORCE)
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

add_custom_target(run
    COMMAND suopTranscriber
    DEPENDS suopTranscriber
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
)
