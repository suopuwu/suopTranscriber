cmake_minimum_required(VERSION 3.21)
project(suopTranscriber VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(SUOP_ENABLE_VULKAN "Build whisper.cpp with Vulkan backend (cross-vendor GPU baseline)" ON)
option(SUOP_ENABLE_CUDA "Build whisper.cpp with CUDA backend (NVIDIA fast path)" OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Multimedia)

qt_standard_project_setup(REQUIRES 6.5)

# whisper.cpp is linked in-process (see DESIGN.md); GGML_VULKAN is the
# cross-vendor floor, GGML_CUDA is an opt-in NVIDIA fast path.
set(GGML_VULKAN ${SUOP_ENABLE_VULKAN} CACHE BOOL "" FORCE)
set(GGML_CUDA ${SUOP_ENABLE_CUDA} CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/whisper.cpp)

qt_add_executable(suopTranscriber
    src/main.cpp
)

qt_add_qml_module(suopTranscriber
    URI SuopTranscriber
    VERSION 1.0
    QML_FILES
        qml/Main.qml
    SOURCES
        src/core/Segment.h
        src/core/TranscriptionEngine.h
        src/core/VaultTreeModel.h
        src/core/VaultTreeModel.cpp
        src/core/AudioPlayer.h
        src/core/AudioPlayer.cpp
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
    whisper
)

add_custom_target(run
    COMMAND suopTranscriber
    DEPENDS suopTranscriber
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
)
