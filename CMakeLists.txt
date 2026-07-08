cmake_minimum_required(VERSION 3.21)
project(suopTranscriber VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(SUOP_ENABLE_VULKAN "Build whisper.cpp with Vulkan backend (cross-vendor GPU baseline)" ON)
option(SUOP_ENABLE_CUDA "Build whisper.cpp with CUDA backend (NVIDIA fast path)" OFF)
option(SUOP_ENABLE_LABELING "Build with llama.cpp for AI auto-labeling" ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Multimedia Core5Compat)
find_package(KF6WindowSystem REQUIRED)

qt_standard_project_setup(REQUIRES 6.7)

# whisper.cpp is linked in-process (see DESIGN.md); GGML_VULKAN is the
# cross-vendor floor, GGML_CUDA is an opt-in NVIDIA fast path.
set(GGML_VULKAN ${SUOP_ENABLE_VULKAN} CACHE BOOL "" FORCE)
set(GGML_CUDA ${SUOP_ENABLE_CUDA} CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/whisper.cpp)

# llama.cpp (AI auto-labeling, see DESIGN.md "Model management"). Vendored
# after whisper.cpp so its CMakeLists' `if (NOT TARGET ggml)` guard finds the
# `ggml` target whisper.cpp already defined and reuses it, instead of trying
# to build a second copy (both projects otherwise vendor their own ggml).
if(SUOP_ENABLE_LABELING)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/llama.cpp)
endif()

qt_add_executable(suopTranscriber
    src/main.cpp
)

# qt_add_qml_module only detects a QML-defined singleton via this source
# property — it does not scan file contents for `pragma Singleton`.
set_source_files_properties(qml/AppSettings.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(suopTranscriber
    URI SuopTranscriber
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/AppSettings.qml
        qml/HoverBorder.qml
        qml/UnderlineField.qml
        qml/TitleBar.qml
        qml/Sidebar.qml
        qml/MainPanel.qml
        qml/BottomBar.qml
        qml/SettingsPopup.qml
    SOURCES
        src/core/Segment.h
        src/core/TranscriptionEngine.h
        src/core/VaultTreeModel.h
        src/core/VaultTreeModel.cpp
        src/core/VaultSearchProxyModel.h
        src/core/VaultSearchProxyModel.cpp
        src/core/AudioPlayer.h
        src/core/AudioPlayer.cpp
        src/core/WindowEffects.h
        src/core/WindowEffects.cpp
        src/core/AudioDecoder.h
        src/core/AudioDecoder.cpp
        src/core/WhisperCppEngine.h
        src/core/WhisperCppEngine.cpp
        src/core/LabelingEngine.h
        src/core/LabelingEngine.cpp
        src/core/ModelManager.h
        src/core/ModelManager.cpp
        src/core/TranscriptionController.h
        src/core/TranscriptionController.cpp
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
    KF6::WindowSystem
    whisper
    llama
)

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
