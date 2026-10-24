add_executable(WinAudioGui
    AppUi.cpp
    ChartDataPipeline.cpp
    DumpUi.cpp
    FrameProbe.cpp
    GuiGpu.cpp
    HeatmapRgba.cpp
    HeatmapTexture.cpp
    WaveEnvelope.cpp
    main.cpp
    Spectrogram.cpp
)
target_include_directories(WinAudioGui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WinAudioGui PRIVATE WinAudioCore implot d3d11 dxgi d3dcompiler shell32)
target_compile_definitions(WinAudioGui PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
# Windows-subsystem exe, but the code has main() (not WinMain): keep the CRT main entry.
set_target_properties(WinAudioGui PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(WinAudioGui PRIVATE /ENTRY:mainCRTStartup)
add_dependencies(WinAudioGui WinAudioHook)
add_custom_command(TARGET WinAudioGui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/channel-idents"
        "$<TARGET_FILE_DIR:WinAudioGui>/channel-idents"
    VERBATIM)
wa_set_project_warnings(WinAudioGui)

# A narrow, opt-in driver of the production GUI; never included in packaging.
get_target_property(WA_GUI_SOURCES WinAudioGui SOURCES)
add_executable(WinAudioPerformanceGui EXCLUDE_FROM_ALL ${WA_GUI_SOURCES} PerformanceFixture.cpp
    ${CMAKE_SOURCE_DIR}/src/probe/PerformanceCalls.cpp)
target_include_directories(WinAudioPerformanceGui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/probe)
target_link_libraries(WinAudioPerformanceGui PRIVATE WinAudioCore implot d3d11 dxgi d3dcompiler shell32)
target_compile_definitions(WinAudioPerformanceGui PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS WA_PERFORMANCE_FIXTURE)
set_target_properties(WinAudioPerformanceGui PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(WinAudioPerformanceGui PRIVATE /ENTRY:mainCRTStartup)
add_custom_command(TARGET WinAudioPerformanceGui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/channel-idents"
        "$<TARGET_FILE_DIR:WinAudioPerformanceGui>/channel-idents"
    VERBATIM)
wa_set_project_warnings(WinAudioPerformanceGui)
