add_executable(WinAudioGui
    AppUi.cpp
    ChartDataPipeline.cpp
    DumpUi.cpp
    main.cpp
    Spectrogram.cpp
)
target_include_directories(WinAudioGui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WinAudioGui PRIVATE WinAudioCore implot d3d11 dxgi d3dcompiler shell32)
target_compile_definitions(WinAudioGui PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
# Windows-subsystem exe, but the code has main() (not WinMain): keep the CRT main entry.
set_target_properties(WinAudioGui PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(WinAudioGui PRIVATE /ENTRY:mainCRTStartup)
wa_set_project_warnings(WinAudioGui)
