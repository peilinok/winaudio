cmake_minimum_required(VERSION 3.20)
project(WinAudioProfile LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(WA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
include("${WA_ROOT}/cmake/CompilerWarnings.cmake")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_executable(CoreProfile core_profile.cpp
    "${WA_ROOT}/src/gui/ChartDataPipeline.cpp"
    "${WA_ROOT}/src/gui/FrameProbe.cpp"
    "${WA_ROOT}/src/gui/Spectrogram.cpp"
    "${WA_ROOT}/src/gui/HeatmapRgba.cpp"
    "${WA_ROOT}/src/gui/WaveEnvelope.cpp"
    "${WA_ROOT}/src/probe/PerformanceCalls.cpp")
target_include_directories(CoreProfile PRIVATE "${WA_ROOT}/src/core" "${WA_ROOT}/src/gui" "${WA_ROOT}/src/probe")
wa_set_project_warnings(CoreProfile)
target_link_libraries(CoreProfile PRIVATE "${WA_ROOT}/build/lib/Release/WinAudioCore.lib"
    ole32 oleaut32 mmdevapi tdh advapi32 shell32)
