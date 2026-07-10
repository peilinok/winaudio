add_executable(WinAudioTests
    test_delayfifo.cpp
    test_streamparams.cpp
    test_smoke.cpp
    test_audioformat.cpp
    test_fft.cpp
    test_ringbuffer.cpp
    test_sampleconvert.cpp
    test_wavfile.cpp
    test_formatspec.cpp
    test_scopebuffer.cpp
    test_analysis.cpp
    test_monitorengine.cpp
    test_spectrogram.cpp
    test_capabilities.cpp
    test_log.cpp
    test_loopback.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/Spectrogram.cpp   # compiled into tests, as in the current setup
)
target_include_directories(WinAudioTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/gui                   # for Spectrogram.h
)
target_link_libraries(WinAudioTests PRIVATE WinAudioCore gtest_main)
wa_set_project_warnings(WinAudioTests)

include(GoogleTest)
gtest_discover_tests(WinAudioTests)
