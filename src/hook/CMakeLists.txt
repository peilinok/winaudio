add_library(WinAudioHook SHARED WinAudioHook.cpp)
target_include_directories(WinAudioHook PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(WinAudioHook PRIVATE ole32 oleaut32 mmdevapi uuid)
wa_set_project_warnings(WinAudioHook)
