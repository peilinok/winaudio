cmake_minimum_required(VERSION 3.20)
project(LogPanelSurvey LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
if(NOT WA_ROOT)
    get_filename_component(WA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
endif()
add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS WA_PERFORMANCE_FIXTURE)
execute_process(COMMAND py -3 "${CMAKE_CURRENT_LIST_DIR}/prepare_fixture.py" "${WA_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}/LogPanelFixture.cpp" COMMAND_ERROR_IS_FATAL ANY)
add_executable(LogPanelSurvey WIN32 "${CMAKE_CURRENT_BINARY_DIR}/LogPanelFixture.cpp"
    "${WA_ROOT}/src/gui/AppUi.cpp" "${WA_ROOT}/src/gui/ChartDataPipeline.cpp"
    "${WA_ROOT}/src/gui/DumpUi.cpp" "${WA_ROOT}/src/gui/FrameProbe.cpp"
    "${WA_ROOT}/src/gui/GuiGpu.cpp" "${WA_ROOT}/src/gui/HeatmapRgba.cpp"
    "${WA_ROOT}/src/gui/HeatmapTexture.cpp" "${WA_ROOT}/src/gui/WaveEnvelope.cpp"
    "${WA_ROOT}/src/gui/main.cpp" "${WA_ROOT}/src/gui/Spectrogram.cpp"
    "${WA_ROOT}/src/probe/PerformanceCalls.cpp")
target_include_directories(LogPanelSurvey PRIVATE "${WA_ROOT}/src/core" "${WA_ROOT}/src/gui"
    "${WA_ROOT}/src/probe" "${WA_ROOT}/third_party/imgui" "${WA_ROOT}/third_party/imgui/backends"
    "${WA_ROOT}/third_party/implot")
target_compile_options(LogPanelSurvey PRIVATE /W4 /permissive- /utf-8 /sdl)
target_link_options(LogPanelSurvey PRIVATE /ENTRY:mainCRTStartup)
target_link_libraries(LogPanelSurvey PRIVATE "${WA_ROOT}/build/lib/Release/WinAudioCore.lib"
    "${WA_ROOT}/build/lib/Release/imgui.lib" "${WA_ROOT}/build/lib/Release/implot.lib"
    ole32 oleaut32 mmdevapi tdh advapi32 shell32 d3d11 dxgi d3dcompiler)
add_custom_command(TARGET LogPanelSurvey POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${WA_ROOT}/assets/channel-idents" "$<TARGET_FILE_DIR:LogPanelSurvey>/channel-idents")
