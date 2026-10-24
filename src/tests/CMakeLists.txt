add_executable(WinAudioTests
    test_delayfifo.cpp
    test_streamparams.cpp
    test_stream_init.cpp
    test_smoke.cpp
    test_audioformat.cpp
    test_fft.cpp
    test_ringbuffer.cpp
    test_sampleconvert.cpp
    test_wavfile.cpp
    test_wav_sink.cpp
    test_formatspec.cpp
    test_scopebuffer.cpp
    test_scope_reader.cpp
    test_analysis.cpp
    test_monitorengine.cpp
    test_capture_track_list.cpp
    test_render_track_list.cpp
    test_production_catalog.cpp
    test_spectrogram.cpp
    test_heatmap_rgba.cpp
    test_heatmap_texture.cpp
    test_wave_envelope.cpp
    test_capture_channel_view.cpp
    test_charts_freeze.cpp
    test_charts_time_zoom.cpp
    test_chart_data_pipeline.cpp
    test_chart_host.cpp
    test_capabilities.cpp
    test_device_enhancements.cpp
    test_audio_effects.cpp
    test_log.cpp
    test_loopback.cpp
    test_audio_session_enumerator.cpp
    test_pipeline_graph.cpp
    test_live_session_list.cpp
    test_shared_probe.cpp
    test_etw_initialize.cpp
    test_hooked_call.cpp
    test_call_log_store.cpp
    test_call_log_filter.cpp
    test_pipeline_split.cpp
    test_user_process_tree.cpp
    test_call_arg_blob.cpp
    test_runtime_fault.cpp
    test_on_demand_attach.cpp
    test_pipeline_attach_ui_model.cpp
    test_pipeline_attach_set.cpp

    test_cli_options.cpp
    test_dump_ui.cpp
    test_frame_probe.cpp
    test_performance_calls.cpp
    test_page_probe.cpp
    test_os_sound_ui.cpp
    test_log_region_prefs.cpp
    test_gui_language.cpp
    test_create_recipe.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/Spectrogram.cpp   # compiled into tests, as in the current setup
    ${CMAKE_SOURCE_DIR}/src/gui/HeatmapRgba.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/HeatmapTexture.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/GuiGpu.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/WaveEnvelope.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/ChartDataPipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/DumpUi.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/FrameProbe.cpp
    ${CMAKE_SOURCE_DIR}/src/probe/PerformanceCalls.cpp
)
target_include_directories(WinAudioTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/cli
    ${CMAKE_SOURCE_DIR}/src/gui                   # for Spectrogram.h
    ${CMAKE_SOURCE_DIR}/src/probe
)
string(REPLACE "\\" "/" WA_CHANNEL_IDENT_DIR "${CMAKE_SOURCE_DIR}/assets/channel-idents")
target_compile_definitions(WinAudioTests PRIVATE "WA_CHANNEL_IDENT_DIR=\"${WA_CHANNEL_IDENT_DIR}\"")
target_link_libraries(WinAudioTests PRIVATE WinAudioCore gtest_main shell32 imgui d3d11)
wa_set_project_warnings(WinAudioTests)

include(GoogleTest)
gtest_discover_tests(WinAudioTests)

# Compile the real hook entry points in an executable without automatic DLL
# installation. Kept independent of WinAudioCore for both x64 and x86 checks.
add_executable(WinAudioHookTests test_hook_buffer.cpp)
target_include_directories(WinAudioHookTests PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(WinAudioHookTests PRIVATE gtest_main ole32 oleaut32 mmdevapi uuid advapi32)
wa_set_project_warnings(WinAudioHookTests)
gtest_discover_tests(WinAudioHookTests)
