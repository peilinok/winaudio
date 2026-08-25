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
    test_spectrogram.cpp
    test_capture_channel_view.cpp
    test_charts_freeze.cpp
    test_charts_time_zoom.cpp
    test_chart_data_pipeline.cpp
    test_chart_host.cpp
    test_capabilities.cpp
    test_log.cpp
    test_loopback.cpp
    test_audio_session_enumerator.cpp
    test_pipeline_graph.cpp
    test_live_session_list.cpp
    test_shared_probe.cpp
    test_etw_initialize.cpp
    test_hooked_call.cpp
    test_on_demand_attach.cpp

    test_cli_options.cpp
    test_dump_ui.cpp
    test_create_recipe.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/Spectrogram.cpp   # compiled into tests, as in the current setup
    ${CMAKE_SOURCE_DIR}/src/gui/ChartDataPipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/DumpUi.cpp
)
target_include_directories(WinAudioTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/cli
    ${CMAKE_SOURCE_DIR}/src/gui                   # for Spectrogram.h
)
target_link_libraries(WinAudioTests PRIVATE WinAudioCore gtest_main shell32)
wa_set_project_warnings(WinAudioTests)

include(GoogleTest)
gtest_discover_tests(WinAudioTests)
