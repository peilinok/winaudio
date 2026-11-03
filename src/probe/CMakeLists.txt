add_executable(LoopbackTreeProbe LoopbackTreeProbe.cpp)
target_link_libraries(LoopbackTreeProbe PRIVATE WinAudioCore)
wa_set_project_warnings(LoopbackTreeProbe)

add_executable(AttachSmoke AttachSmoke.cpp)
target_link_libraries(AttachSmoke PRIVATE WinAudioCore)
wa_set_project_warnings(AttachSmoke)

add_executable(AttachVolumeProbe AttachVolumeProbe.cpp)
target_link_libraries(AttachVolumeProbe PRIVATE WinAudioCore)
wa_set_project_warnings(AttachVolumeProbe)

add_executable(HookBufferProbe HookBufferProbe.cpp)
target_include_directories(HookBufferProbe PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(HookBufferProbe PRIVATE ole32 oleaut32 mmdevapi uuid advapi32)
wa_set_project_warnings(HookBufferProbe)

add_executable(HookAudioProbe HookAudioProbe.cpp)
target_link_libraries(HookAudioProbe PRIVATE WinAudioCore)
wa_set_project_warnings(HookAudioProbe)

add_executable(PipelineIntegrationProbe PipelineIntegrationProbe.cpp)
target_link_libraries(PipelineIntegrationProbe PRIVATE WinAudioCore)
wa_set_project_warnings(PipelineIntegrationProbe)

add_executable(HookTransportProbe HookTransportProbe.cpp)
target_link_libraries(HookTransportProbe PRIVATE WinAudioCore)
wa_set_project_warnings(HookTransportProbe)
add_test(NAME HookTransport.CrossProcessSynthetic
    COMMAND HookTransportProbe --output "${CMAKE_CURRENT_BINARY_DIR}/hook-transport-$<CONFIG>.json")
set_tests_properties(HookTransport.CrossProcessSynthetic PROPERTIES TIMEOUT 45)

add_executable(SegmentWindowProbe SegmentWindowProbe.cpp)
target_link_libraries(SegmentWindowProbe PRIVATE WinAudioCore oleaut32)
wa_set_project_warnings(SegmentWindowProbe)
add_test(NAME SegmentWindow.CrossProcessCompletedCalls
    COMMAND SegmentWindowProbe --output "${CMAKE_CURRENT_BINARY_DIR}/segment-window-$<CONFIG>.json")
set_tests_properties(SegmentWindow.CrossProcessCompletedCalls PROPERTIES TIMEOUT 60)
