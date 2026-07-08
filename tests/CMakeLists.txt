# Test suite = living spec sheet. Each tst_* documents expected behaviors from
# DESIGN.md / CLAUDE.md / StyleGuide.md. Documented-but-unbuilt behaviors live
# in tst_future_spec as QSKIP placeholders so every ctest run lists them.

find_package(Qt6 6.7 REQUIRED COMPONENTS Test)

set(SUOP_CORE_DIR ${CMAKE_SOURCE_DIR}/src/core)

# Each test compiles the core sources it needs directly — no core static lib,
# so the app target stays untouched. QML_ELEMENT headers compile outside the
# qml module as long as Qt6::Qml is linked.
function(suop_add_test name)
    cmake_parse_arguments(ARG "LINK_WHISPER;LINK_LLAMA;LINK_SHERPA_ONNX" "" "SOURCES" ${ARGN})
    set(abs_sources "")
    foreach(src ${ARG_SOURCES})
        list(APPEND abs_sources ${SUOP_CORE_DIR}/${src})
    endforeach()
    qt_add_executable(${name} ${name}.cpp ${abs_sources})
    target_include_directories(${name} PRIVATE ${SUOP_CORE_DIR})
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
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
suop_add_test(tst_vaulttreemodel SOURCES VaultTreeModel.cpp)
suop_add_test(tst_vaultsearchproxymodel SOURCES VaultTreeModel.cpp VaultSearchProxyModel.cpp)
suop_add_test(tst_modelmanager SOURCES ModelManager.cpp)
suop_add_test(tst_transcript_format LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp)
suop_add_test(tst_transcription_queue LINK_WHISPER LINK_LLAMA LINK_SHERPA_ONNX
    SOURCES TranscriptionController.cpp WhisperCppEngine.cpp ModelManager.cpp AudioDecoder.cpp LabelingEngine.cpp DiarizationEngine.cpp)
suop_add_test(tst_engine_contract LINK_WHISPER
    SOURCES WhisperCppEngine.cpp AudioDecoder.cpp)
suop_add_test(tst_audiodecoder SOURCES AudioDecoder.cpp)
suop_add_test(tst_labelingengine LINK_LLAMA SOURCES LabelingEngine.cpp)
suop_add_test(tst_diarizationengine LINK_SHERPA_ONNX SOURCES DiarizationEngine.cpp AudioDecoder.cpp)
suop_add_test(tst_future_spec)
suop_add_test(tst_styleguide)
