add_executable(WinAudioCli main.cpp)   # console subsystem is the default (no WIN32)
target_link_libraries(WinAudioCli PRIVATE WinAudioCore)  # inherits src/core include + ole32/oleaut32
wa_set_project_warnings(WinAudioCli)
