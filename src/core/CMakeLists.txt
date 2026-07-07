add_library(WinAudioCore STATIC
    DelayFifo.cpp
    AudioFormat.cpp
    Fft.cpp
    RingBuffer.cpp
    SampleConvert.cpp
    WavFile.cpp
    DeviceEnumerator.cpp
    WasapiStream.cpp
    Engine.cpp
    FormatSpec.cpp
    MonitorEngine.cpp
    Capabilities.cpp
    Log.cpp
)
target_include_directories(WinAudioCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WinAudioCore PUBLIC ole32 oleaut32 spdlog)
wa_set_project_warnings(WinAudioCore)
